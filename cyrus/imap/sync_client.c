/* sync_client.c -- Cyrus synchonization client
 *
 * Copyright (c) 1998-2005 Carnegie Mellon University.  All rights reserved.
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
 * Original version written by David Carter <dpc22@cam.ac.uk>
 * Rewritten and integrated into Cyrus by Ken Murchison <ken@oceana.com>
 *
 * $Id: sync_client.c,v 1.1.2.1 2005/02/21 19:25:46 ken3 Exp $
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <com_err.h>
#include <syslog.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

#include "global.h"
#include "assert.h"
#include "mboxlist.h"
#include "exitcodes.h"
#include "imap_err.h"
#include "mailbox.h"
#include "quota.h"
#include "xmalloc.h"
#include "acl.h"
#include "seen.h"
#include "mboxname.h"
#include "map.h"
#include "imapd.h"
#include "imparse.h"
#include "util.h"
#include "prot.h"
#include "sync_support.h"
#include "sync_commit.h"
#include "lock.h"
#include "backend.h"

/* signal to config.c */
const int config_need_data = 0;  /* YYY */

/* Stuff to make index.c link */
int imapd_exists;
struct protstream *imapd_out = NULL;
struct auth_state *imapd_authstate = NULL;
char *imapd_userid = NULL;

void printastring(const char *s)
{
    fatal("not implemented", EC_SOFTWARE);
}

void printstring(const char *s)
{
    fatal("not implemented", EC_SOFTWARE);
}

/* end stuff to make index.c link */

/* ====================================================================== */

/* Static global variables and support routines for sync_client */

extern char *optarg;
extern int optind;

static struct protstream *toserver   = NULL;
static struct protstream *fromserver = NULL;

/* List/Hash of messageIDs that are available on server */
static struct sync_msgid_list *msgid_onserver = NULL;

static struct namespace   sync_namespace;
static struct auth_state *sync_authstate = NULL;

static int verbose         = 0;
static int verbose_logging = 0;

static void shut_down(int code) __attribute__((noreturn));
static void shut_down(int code)
{
    seen_done();
    quotadb_close();
    quotadb_done();
    mboxlist_close();
    mboxlist_done();
    exit(code);
}

static int usage(const char *name)
{
    fprintf(stderr,
            "usage: %s -S <servername> [-C <alt_config>] [-r] [-v] mailbox...\n", name);
 
    exit(EC_USAGE);
}

void fatal(const char *s, int code)
{
    fprintf(stderr, "sync_client: %s\n", s);
    exit(code);
}

/* ====================================================================== */

static int
send_user(char *user)
{
    prot_printf(toserver, "USER "); 
    sync_printastring(toserver, user);
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    return(sync_parse_code("USER", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
send_enduser()
{
    int r = 0;
    int c = ' ';
    static struct buf token;   /* BSS */

    prot_printf(toserver, "ENDUSER\r\n"); 
    prot_flush(toserver);

    r = sync_parse_code("ENDUSER", fromserver, SYNC_PARSE_NOEAT_OKLINE, NULL);
    if (r) return(r);

    if ((c = getword(fromserver, &token)) != ' ') {
        eatline(fromserver, c);
        syslog(LOG_ERR, "Garbage on Enduser response");
        return(IMAP_PROTOCOL_ERROR);
    }
    eatline(fromserver, c);

    /* Clear out msgid_on_server list if server restarted */
    if (!strcmp(token.s, "[RESTART]")) {
        int hash_size = msgid_onserver->hash_size;

        sync_msgid_list_free(&msgid_onserver);
        msgid_onserver = sync_msgid_list_create(hash_size);
    }

    return(0);
}

/* ====================================================================== */

/* Routines relevant to reserve operation */

/* Find the messages that we will want to upload from this mailbox,
 * flag messages that are already available at the server end */

static int
find_reserve_messages(struct mailbox *mailbox,
                      struct sync_msg_list   *msg_list,
                      struct sync_msgid_list *server_msgid_list,
                      struct sync_msgid_list *reserve_msgid_list)
{
    struct sync_msg *msg;
    struct index_record record;
    unsigned long msgno;
    int r;

    if (mailbox->exists == 0)
        return(0);

    msg = msg_list->head;
    for (msgno = 1 ; msgno <= mailbox->exists ; msgno++) {
        r = mailbox_read_index_record(mailbox, msgno, &record);

        if (r) {
            syslog(LOG_ERR,
                   "IOERROR: reading index entry for nsgno %lu of %s: %m",
                   record.uid, mailbox->name);
            return(IMAP_IOERROR);
        }

        if (msg && ((msg->uid < record.uid) ||
                    ((msg->uid == record.uid) &&
                     message_uuid_compare(&msg->uuid, &record.uuid)))) {
            msg = msg->next;
            continue;
        }

        /* Want to upload this message; does the server have a copy? */
        if (sync_msgid_lookup(server_msgid_list, &record.uuid))
            sync_msgid_add(reserve_msgid_list, &record.uuid);
    }
    
    return(0);
}

static int
reserve_all_messages(struct mailbox *mailbox,
                     struct sync_msgid_list *server_msgid_list,
                     struct sync_msgid_list *reserve_msgid_list)
{
    struct index_record record;
    unsigned long msgno;
    int r;

    if (mailbox->exists == 0)
        return(0);

    for (msgno = 1 ; msgno <= mailbox->exists ; msgno++) {
        r = mailbox_read_index_record(mailbox, msgno, &record);

        if (r) {
            syslog(LOG_ERR,
                   "IOERROR: reading index entry for nsgno %lu of %s: %m",
                   record.uid, mailbox->name);
            return(IMAP_IOERROR);
        }

        /* Want to upload this message; does the server have a copy? */
        if (sync_msgid_lookup(server_msgid_list, &record.uuid))
            sync_msgid_add(reserve_msgid_list, &record.uuid);
    }
    
    return(0);
}

/* Count numbers of instances on server of each MessageID that we will
 * want to copy */

static int
count_reserve_messages(struct sync_folder *server_folder,
                       struct sync_msgid_list *reserve_msgid_list)
{
    struct sync_msg_list *msglist = server_folder->msglist;
    struct sync_msg      *msg;
    struct sync_msgid    *msgid;

    for (msg = msglist->head ; msg ; msg = msg->next) {
        if ((msgid=sync_msgid_lookup(reserve_msgid_list, &msg->uuid)))
            msgid->count++;
    }
    
    return(0);
}

static int
reserve_check_folder(struct sync_msgid_list *reserve_msgid_list,
                     struct sync_folder     *folder)
{
    struct sync_msg   *msg;
    struct sync_msgid *msgid;

    for (msg = folder->msglist->head ; msg ; msg = msg->next) {
        msgid = sync_msgid_lookup(reserve_msgid_list, &msg->uuid);

        if (msgid && !msgid->reserved)
            return(1);
    }
    return(0);
}

static int
reserve_folder(struct sync_msgid_list *reserve_msgid_list,
               struct sync_folder     *folder)
{
    struct sync_msg   *msg;
    struct sync_msgid *msgid;
    static struct buf arg;
    int r = 0, unsolicited, c;

    prot_printf(toserver, "RESERVE "); 
    sync_printastring(toserver, folder->name);

    for (msg = folder->msglist->head ; msg ; msg = msg->next) {
        msgid = sync_msgid_lookup(reserve_msgid_list, &msg->uuid);

        if (msgid && !msgid->reserved) {
            /* Attempt to Reserve message in this folder */
            prot_printf(toserver, " "); 
            sync_printastring(toserver, message_uuid_text(&msgid->uuid));
        }
    }
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    r = sync_parse_code("RESERVE", fromserver,
                        SYNC_PARSE_EAT_OKLINE, &unsolicited);

    /* Parse response to record successfully reserved messages */
    while (!r && unsolicited) {
        struct message_uuid tmp_uuid;

        c = getword(fromserver, &arg);

        if (c == '\r')
            c = prot_getc(fromserver);

        if (c != '\n') {
            syslog(LOG_ERR, "Illegal response to RESERVE: %s", arg.s);
            sync_eatlines_unsolicited(fromserver, c);
            return(IMAP_PROTOCOL_ERROR);
        }
 
        if (!message_uuid_from_text(&tmp_uuid, arg.s)) {
            syslog(LOG_ERR, "Illegal response to RESERVE: %s", arg.s);
            sync_eatlines_unsolicited(fromserver, c);
            return(IMAP_PROTOCOL_ERROR);
        }

        if ((msgid = sync_msgid_lookup(reserve_msgid_list, &tmp_uuid))) {
            msgid->reserved = 1;
            reserve_msgid_list->reserved++;
            sync_msgid_add(msgid_onserver, &tmp_uuid);
        } else
            syslog(LOG_ERR, "RESERVE: Unexpected response MessageID %s in %s",
                   arg.s, folder->name);

        r = sync_parse_code("RESERVE", fromserver,
                            SYNC_PARSE_EAT_OKLINE, &unsolicited);
    }
    return(r);
}

struct reserve_sort_item {
    struct sync_folder *folder;
    unsigned long count;
};

static int
reserve_folder_compare(const void *v1, const void *v2)
{
    struct reserve_sort_item *s1 = (struct reserve_sort_item *)v1;
    struct reserve_sort_item *s2 = (struct reserve_sort_item *)v2;

    return(s1->count - s2->count);
}

static int
reserve_messages(struct sync_folder_list *client_list,
                 struct sync_folder_list *server_list,
                 int *vanishedp)
{
    struct sync_msgid_list *server_msgid_list  = NULL;
    struct sync_msgid_list *reserve_msgid_list = NULL;
    struct sync_folder     *folder, *folder2;
    struct sync_msg   *msg;
    struct sync_msgid *msgid;
    struct reserve_sort_item *reserve_sort_list = 0;
    int reserve_sort_count;
    int r = 0;
    int mailbox_open = 0;
    int count;
    int i;
    struct mailbox m;

    server_msgid_list  = sync_msgid_list_create(SYNC_MSGID_LIST_HASH_SIZE);
    reserve_msgid_list = sync_msgid_list_create(SYNC_MSGID_LIST_HASH_SIZE);

    /* Generate fast lookup hash of all MessageIDs available on server */
    for (folder = server_list->head ; folder ; folder = folder->next) {
        for (msg = folder->msglist->head ; msg ; msg = msg->next) {
            if (!sync_msgid_lookup(server_msgid_list, &msg->uuid))
                sync_msgid_add(server_msgid_list, &msg->uuid);
        }
    }

    /* Find messages we want to upload that are available on server */
    for (folder = client_list->head ; folder ; folder = folder->next) {
        folder->id  = NULL;
        folder->acl = NULL;

        r = mailbox_open_header(folder->name, 0, &m);

        /* Quietly skip over folders which have been deleted since we
           started working (but record fact in case caller cares) */
        if (r == IMAP_MAILBOX_NONEXISTENT) {  
            (*vanishedp)++;
            r = 0;     
            continue;
        }

        /* Quietly ignore objects that we don't have access to.
         * Includes directory stubs, which have not underlying cyrus.*
         * files in the filesystem */
        if (r == IMAP_PERMISSION_DENIED) {
            r = 0;
            continue;
        }

        if (!r) mailbox_open = 1;
        if (!r) r = mailbox_open_index(&m);

        if (r) {
            if (mailbox_open) mailbox_close(&m);

            syslog(LOG_ERR, "IOERROR: %s", error_message(r));
            goto bail;
        }

        folder->id  = xstrdup(m.uniqueid);
        folder->acl = xstrdup(m.acl);

        if ((folder2=sync_folder_lookup(server_list, m.uniqueid)))
            find_reserve_messages(&m, folder2->msglist, 
                                  server_msgid_list,
                                  reserve_msgid_list);
        else
            reserve_all_messages(&m, 
                                 server_msgid_list,
                                 reserve_msgid_list);

        mailbox_close(&m);
    }

    if (reserve_msgid_list->count == 0) {
        r = 0;      /* Nothing to do */
        goto bail;
    }

    /* Generate instance count for messages available on server */
    for (folder = server_list->head ; folder ; folder = folder->next)
        count_reserve_messages(folder, reserve_msgid_list);

    /* Add all folders which have unique copies of messages to reserve list
     * (as they will definitely be needed) */
    for (folder = server_list->head ; folder ; folder = folder->next) {
        for (msg = folder->msglist->head ; msg ; msg = msg->next) {
            msgid = sync_msgid_lookup(reserve_msgid_list, &msg->uuid);

            if (msgid && (msgid->count == 1)) {
                reserve_folder(reserve_msgid_list, folder);
                folder->reserve = 1;
                break;
            }
        }
    }

    /* Record all folders with unreserved messages and sort them so the
     * folder with most unreserved messages in first */
    reserve_sort_list
        = xmalloc(server_list->count*sizeof(struct reserve_sort_item));

    /* Count messages we will be able to reserve from each folder on server */
    reserve_sort_count = 0;
    for (folder = server_list->head; folder ; folder=folder->next) {
        if (folder->reserve) continue;

        for (count = 0, msg = folder->msglist->head ; msg ; msg = msg->next) {
            msgid = sync_msgid_lookup(reserve_msgid_list, &msg->uuid);

            if (msgid && !msgid->reserved)
                count++;
        }

        if (count > 0) {
            reserve_sort_list[reserve_sort_count].folder = folder;
            reserve_sort_list[reserve_sort_count].count  = count;
            reserve_sort_count++;
        }
    }

    /* Sort folders (folder with most reservable messages first) */
    if (reserve_sort_count > 0)
        qsort(reserve_sort_list, reserve_sort_count,
              sizeof(struct reserve_sort_item), reserve_folder_compare);

    /* Work through folders until all messages reserved or no more */
    for (i=0; i < reserve_sort_count ; i++) {
        folder = reserve_sort_list[i].folder;

        if (reserve_check_folder(reserve_msgid_list, folder))
            reserve_folder(reserve_msgid_list, folder);

        if (reserve_msgid_list->reserved == reserve_msgid_list->count)
            break;
    }

 bail:
    sync_msgid_list_free(&server_msgid_list);
    sync_msgid_list_free(&reserve_msgid_list);
    if (reserve_sort_list) free(reserve_sort_list);
    return(r);
}

static int
folders_get_uniqueid(struct sync_folder_list *client_list, int *vanishedp)
{
    struct sync_folder *folder;
    int r = 0;
    int mailbox_open = 0;
    struct mailbox m;

    /* Find messages we want to upload that are available on server */
    for (folder = client_list->head ; folder ; folder = folder->next) {
        folder->id  = NULL;
        folder->acl = NULL;

        r = mailbox_open_header(folder->name, 0, &m);

        /* Quietly skip over folders which have been deleted since we
           started working (but record fact in case caller cares) */
        if (r == IMAP_MAILBOX_NONEXISTENT) {
            (*vanishedp)++;
            r = 0;
            continue;
        }

        /* Quietly ignore objects that we don't have access to.
         * Includes directory stubs, which have not underlying cyrus.*
         * files in the filesystem */
        if (r == IMAP_MAILBOX_NONEXISTENT) {
            r = 0;
            continue;
        }


        if (!r) mailbox_open = 1;
        if (!r) r = mailbox_open_index(&m);

       if (r) {
            if (mailbox_open) mailbox_close(&m);
            syslog(LOG_ERR, "IOERROR: %s", error_message(r));
            return(r);
        }

        folder->id  = xstrdup(m.uniqueid);
        folder->acl = xstrdup(m.acl);

        mailbox_close(&m);
    }

    return(0);
}

/* ====================================================================== */

static int
user_reset(char *user)
{
    prot_printf(toserver, "RESET "); 
    sync_printastring(toserver, user);
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    return(sync_parse_code("RESET", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

/* ====================================================================== */

static int
folder_select(char *name, char *myuniqueid,
              unsigned long *lastuidp, time_t *lastseenp)
{
    int r, c;
    static struct buf uniqueid;
    static struct buf lastuid;
    static struct buf lastseen;
    static struct buf last_recent_uid;

    /* XXX Make Separate version which doesn't care about lastuid/lastseen! */

    prot_printf(toserver, "SELECT "); 
    sync_printastring(toserver, name);
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    r = sync_parse_code("SELECT", fromserver, SYNC_PARSE_NOEAT_OKLINE, NULL);
    if (r) return(r);
    
    if ((c = getword(fromserver, &uniqueid)) != ' ') {
        eatline(fromserver, c);
        syslog(LOG_ERR, "Garbage on Select response");
        return(IMAP_PROTOCOL_ERROR);
    }

    c = getword(fromserver, &lastuid);
    if (c != ' ') {
        eatline(fromserver, c);
        syslog(LOG_ERR, "Garbage on Select response");
        return(IMAP_PROTOCOL_ERROR);
    }
    c = getword(fromserver, &lastseen);
    if (c != ' ') {
        eatline(fromserver, c);
        syslog(LOG_ERR, "Garbage on Select response");
        return(IMAP_PROTOCOL_ERROR);
    }
    c = getword(fromserver, &last_recent_uid);
    
    if (c == '\r') c = prot_getc(fromserver);
    if (c != '\n') {
        eatline(fromserver, c);
        syslog(LOG_ERR, "Garbage on Select response");
        return(IMAP_PROTOCOL_ERROR);
    }

    if (strcmp(uniqueid.s, myuniqueid) != 0)
        return(IMAP_MAILBOX_MOVED);

    if (lastuidp)  *lastuidp  = sync_atoul(lastuid.s);
    if (lastseenp) *lastseenp = sync_atoul(lastseen.s);
    return(0);
}

static int
folder_create(char *name, char *uniqueid, char *acl, unsigned long uidvalidity)
{
    prot_printf(toserver, "CREATE ");
    sync_printastring(toserver, name);
    prot_printf(toserver, " ");
    sync_printastring(toserver, uniqueid);
    prot_printf(toserver, " ");
    sync_printastring(toserver, acl);
    prot_printf(toserver, " ");
    prot_printf(toserver, "0");
    prot_printf(toserver, " ");
    prot_printf(toserver, "%lu\r\n", uidvalidity);
    prot_flush(toserver);

    return(sync_parse_code("CREATE", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
folder_rename(char *oldname, char *newname)
{
    prot_printf(toserver, "RENAME ");
    sync_printastring(toserver, oldname);
    prot_printf(toserver, " ");
    sync_printastring(toserver, newname);
    prot_printf(toserver, "\r\n");
    prot_flush(toserver);

    return(sync_parse_code("RENAME", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
folder_delete(char *name)
{
    prot_printf(toserver, "DELETE "); 
    sync_printastring(toserver, name);
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    return(sync_parse_code("DELETE", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
folder_addsub(char *name)
{
    prot_printf(toserver, "ADDSUB ");
    sync_printastring(toserver, name);
    prot_printf(toserver, "\r\n");
    prot_flush(toserver);

    return(sync_parse_code("ADDSUB", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
folder_delsub(char *name)
{
    prot_printf(toserver, "DELSUB ");
    sync_printastring(toserver, name);
    prot_printf(toserver, "\r\n");
    prot_flush(toserver);

    return(sync_parse_code("DELSUB", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
folder_setacl(char *name, char *acl)
{
    prot_printf(toserver, "SETACL "); 
    sync_printastring(toserver, name);
    prot_printf(toserver, " "); 
    sync_printastring(toserver, acl);
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    return(sync_parse_code("SETACL", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

/* ====================================================================== */

static int
sieve_upload(char *user, char *name, unsigned long last_update)
{
    char *s, *sieve;
    unsigned long size;

    if (!(sieve = sync_sieve_read(user, name, &size))) {
        return(IMAP_IOERROR);
    }

    prot_printf(toserver, "UPLOAD_SIEVE "); 
    sync_printastring(toserver, name);
    prot_printf(toserver, " %lu {%lu+}\r\n", last_update, size);

    s = sieve;
    while (size) {
        prot_putc(*s, toserver);
        s++;
        size--;
    }
    prot_printf(toserver,"\r\n");
    free(sieve);
    prot_flush(toserver);

    return(sync_parse_code("UPLOAD_SIEVE",
                           fromserver, SYNC_PARSE_EAT_OKLINE, NULL));

    return(1);
}

static int
sieve_delete(char *name)
{
    prot_printf(toserver, "DELETE_SIEVE "); 
    sync_printastring(toserver, name);
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    return(sync_parse_code("DELETE_SIEVE",
                           fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
sieve_activate(char *name)
{
    prot_printf(toserver, "ACTIVATE_SIEVE "); 
    sync_printastring(toserver, name);
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    return(sync_parse_code("ACTIVATE_SIEVE",
                           fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
sieve_deactivate()
{
    prot_printf(toserver, "DEACTIVATE_SIEVE\r\n"); 
    prot_flush(toserver);

    return(sync_parse_code("DEACTIVATE_SIEVE",
                           fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

/* ====================================================================== */

static int
update_quota_work(char *user, struct mailbox *mailbox, struct quota *quota)
{
    char tmp[MAX_MAILBOX_NAME+1];
    int  r;
    struct txn *tid = NULL;

    if ((r = quota_read(&mailbox->quota, &tid, 0))) {
        syslog(LOG_INFO, "Warning: failed to read mailbox quota for %s: %s",
               user, error_message(r));
        return(0);
    }

    if (mailbox->quota.limit == quota->limit)
        return(0);

    prot_printf(toserver, "SETQUOTA ");
    sprintf(tmp, "user.%s", user);
    sync_printastring(toserver, tmp);

    prot_printf(toserver, " %d\r\n", mailbox->quota.limit);
    prot_flush(toserver);
    
    return(sync_parse_code("SETQUOTA",fromserver,SYNC_PARSE_EAT_OKLINE,NULL));
}

/* ====================================================================== */

static void
create_flags_lookup(int table[], char *client[], char *server[])
{
    int i, j;

    /* Rather unfortunate O(n^2) loop, where 0 <= n <= 128
     * However n (number of active user defined flags is typically small:
     * (client[i] == NULL) test should make this much closer to O(n).
     */
    for (i = 0 ; i < MAX_USER_FLAGS ; i++) {
        table[i] = (-1);

        if (client[i] == NULL)
            continue;

        for (j = 0 ; j < MAX_USER_FLAGS ; j++) {
            if (server[j] && !strcmp(client[i], server[j])) {
                table[i] = j;
                break;
            }
        }
    }
}

static int
check_flags(struct mailbox *mailbox, struct sync_msg_list *list,
            int flag_lookup_table[])
{
    struct sync_msg *msg;
    unsigned long msgno;
    struct index_record record;
    int cflag, sflag, cvalue, svalue;

    msg = list->head;
    for (msgno = 1; msg && (msgno <= mailbox->exists) ; msgno++) {
        mailbox_read_index_record(mailbox, msgno, &record);

        /* Skip msgs on client missing on server (will upload later) */
        if (record.uid < msg->uid)
            continue;

        /* Skip over messages recorded on server which are missing on client
         * (either will be expunged or have been expunged already) */
        while (msg && (record.uid > msg->uid))
            msg = msg->next;

        if (!(msg && (record.uid == msg->uid)))
            continue;

        /* Got a message on client which has same UID as message on server
         * Work out if system and user flags match */
        if (record.system_flags != msg->flags.system_flags)
            return(1);

        for (cflag = 0; cflag < MAX_USER_FLAGS; cflag++) {
            if (mailbox->flagname[cflag] == NULL)
                continue;

            cvalue = svalue = 0;

            if (record.user_flags[cflag/32] & (1<<(cflag&31)))
                cvalue = 1;

            if (((sflag = flag_lookup_table[cflag]) >= 0) &&
                (msg->flags.user_flags[sflag/32] & 1<<(sflag&31)))
                svalue = 1;

            if (cvalue != svalue)
                return(1);
        }
    }
    return(0);
}

static int
update_flags(struct mailbox *mailbox, struct sync_msg_list *list,
             int flag_lookup_table[])
{
    struct sync_msg *msg;
    unsigned long msgno;
    struct index_record record;
    int flags_printed, flag;
    int cflag, sflag, cvalue, svalue;
    int update;
    int have_update = 0;

    msg = list->head;
    for (msgno = 1; msg && (msgno <= mailbox->exists) ; msgno++) {
        mailbox_read_index_record(mailbox, msgno, &record);

        /* Skip msgs on client missing on server (will upload later) */
        if (record.uid < msg->uid)
            continue;
        
        /* Skip over messages recorded on server which are missing on client
         * (either will be expunged or have been expunged already) */
        while (msg && (record.uid > msg->uid))
            msg = msg->next;

        if (!(msg && (record.uid == msg->uid)))
            continue;

        /* Got a message on client which has same UID as message on server
         * Work out if system and user flags match */
        update = 0;
        if (record.system_flags != msg->flags.system_flags) {
            update = 1;
        } else for (cflag = 0; cflag < MAX_USER_FLAGS; cflag++) {
            if (mailbox->flagname[cflag] == NULL)
                continue;

            cvalue = svalue = 0;
            
            if (record.user_flags[cflag/32] & (1<<(cflag&31)))
                cvalue = 1;
                    
            if (((sflag = flag_lookup_table[cflag]) >= 0) &&
                (msg->flags.user_flags[sflag/32] & 1<<(sflag&31)))
                svalue = 1;
                    
            if (cvalue != svalue) {
                update = 1;
                break;
            }
        }
        if (!update)
            continue;

        if (!have_update) {
            prot_printf(toserver, "SETFLAGS");
            have_update = 1;
        }

        prot_printf(toserver, " %lu (", record.uid);
        flags_printed = 0;

        if (record.system_flags & FLAG_DELETED)
            sync_flag_print(toserver, &flags_printed,"\\deleted");
        if (record.system_flags & FLAG_ANSWERED)
            sync_flag_print(toserver, &flags_printed,"\\answered");
        if (record.system_flags & FLAG_FLAGGED)
            sync_flag_print(toserver,&flags_printed, "\\flagged");
        if (record.system_flags & FLAG_DRAFT)
            sync_flag_print(toserver,&flags_printed, "\\draft");
        
        for (flag = 0 ; flag < MAX_USER_FLAGS ; flag++) {
            if (mailbox->flagname[flag] &&
                (record.user_flags[flag/32] & (1<<(flag&31)) ))
                sync_flag_print(toserver, &flags_printed,
                                mailbox->flagname[flag]);
        }
        prot_printf(toserver, ")");
    }

    if (!have_update)
        return(0);

    prot_printf(toserver, "\r\n");
    prot_flush(toserver);

    return(sync_parse_code("SETFLAGS",fromserver,SYNC_PARSE_EAT_OKLINE,NULL));
}

/* ====================================================================== */

static int
check_expunged(struct mailbox *mailbox, struct sync_msg_list *list)
{
    struct sync_msg *msg = list->head;
    unsigned long msgno = 1;
    struct index_record record;

    for (msgno = 1; msg && (msgno <= mailbox->exists) ; msgno++) {
        mailbox_read_index_record(mailbox, msgno, &record);

        /* Skip msgs on client missing on server (will upload later) */
        if (record.uid < msg->uid)
            continue;

        /* Message on server doesn't exist on client: need expunge */
        if (record.uid > msg->uid)
            return(1);

        /* UIDs match => exist on client and server */
        msg = msg->next;
    }
    return((msg) ? 1 : 0);  /* Remaining messages on server: expunge needed */
}

static int
expunge(struct mailbox *mailbox, struct sync_msg_list *list)
{
    struct sync_msg *msg = list->head;
    unsigned long msgno = 1;
    struct index_record record;
    int count = 0;

    for (msgno = 1; msg && (msgno <= mailbox->exists) ; msgno++) {
        mailbox_read_index_record(mailbox, msgno, &record);

        /* Skip msgs on client missing on server (will upload later) */
        if (record.uid < msg->uid)
            continue;

        /* Expunge messages on server which do not exist on client */
        while (msg && (record.uid > msg->uid)) {
            if (count++ == 0)
                prot_printf(toserver, "EXPUNGE");

            prot_printf(toserver, " %lu", msg->uid);
            msg = msg->next;
        }

        /* Skip messages which exist on both client and server */
        if (msg && (record.uid == msg->uid))
            msg = msg->next;
    }

    /* Expunge messages on server which do not exist on client */
    while (msg) {
        if (count++ == 0)
            prot_printf(toserver, "EXPUNGE");

        prot_printf(toserver, " %lu", msg->uid);

        msg = msg->next;
    }

    if (count == 0)
        return(0);

    prot_printf(toserver, "\r\n");
    prot_flush(toserver);
    return(sync_parse_code("EXPUNGE",fromserver,SYNC_PARSE_EAT_OKLINE,NULL));
}

/* ====================================================================== */

/* Check whether there are any messages to upload in this folder */

static int
check_upload_messages(struct mailbox *mailbox, struct sync_msg_list *list)
{
    struct sync_msg *msg;
    struct index_record record;
    unsigned long msgno;

    if (mailbox->exists == 0)
        return(0);

    /* Find out whether server is missing any messages */
    if ((msg = list->head) == NULL)
        return(1);

    for (msgno = 1 ; msgno <= mailbox->exists ; msgno++) {
        if (mailbox_read_index_record(mailbox, msgno, &record))
            return(1);     /* Attempt upload, report error then */
        
        /* Skip over messages recorded on server which are missing on client
         * (either will be expunged or have been expunged already) */
        while (msg && (record.uid > msg->uid))
            msg = msg->next;

        if (msg && (record.uid == msg->uid) &&
            message_uuid_compare(&record.uuid, &msg->uuid)) {
            msg = msg->next;  /* Ignore exact match */
            continue;
        }

        /* Found a message on the client which doesn't exist on the server */
        return(1);
    }

    return (msgno <= mailbox->exists);
}

/* Upload missing messages from folders (uses UPLOAD COPY where possible) */

static int
upload_message_work(struct mailbox *mailbox,
                    unsigned long msgno, struct index_record *record)
{
    unsigned long cache_size;
    int flags_printed = 0;
    int r = 0, flag, need_body;
    static unsigned long sequence = 1;
    const char *msg_base = NULL;
    unsigned long msg_size = 0;

    /* Protocol for PARSED items:
     * C:  PARSED  <msgid> <uid> 
     *             <internaldate> <sent-date> <last-updated> <flags>
     *             <hdr size> <content_lines>
     *             <cache literal (includes cache size!)>
     * <msg literal (includes msg size!)>
     */

    /* Protocol for COPY items:
     * C:  COPY <msgid> <uid>
     *           <internaldate> <sent-date> <last-updated> <flags>
     */

    if (sync_msgid_lookup(msgid_onserver, &record->uuid)) {
        prot_printf(toserver, " COPY");
        need_body = 0;
    } else {
        sync_msgid_add(msgid_onserver, &record->uuid);
        prot_printf(toserver, " PARSED");
        need_body = 1;
    }

    prot_printf(toserver, " %s %lu %lu %lu %lu (",
             message_uuid_text(&record->uuid),
             record->uid, record->internaldate,
             record->sentdate, record->last_updated);

    flags_printed = 0;

    if (record->system_flags & FLAG_DELETED)
        sync_flag_print(toserver, &flags_printed, "\\deleted");
    if (record->system_flags & FLAG_ANSWERED)
        sync_flag_print(toserver, &flags_printed, "\\answered");
    if (record->system_flags & FLAG_FLAGGED)
        sync_flag_print(toserver, &flags_printed, "\\flagged");
    if (record->system_flags & FLAG_DRAFT)
        sync_flag_print(toserver, &flags_printed, "\\draft");

    for (flag = 0 ; flag < MAX_USER_FLAGS ; flag++) {
        if (mailbox->flagname[flag] &&
            (record->user_flags[flag/32] & (1<<(flag&31)) ))
            sync_flag_print(toserver, 
                            &flags_printed, mailbox->flagname[flag]);
    }
    prot_printf(toserver, ")");

    if (need_body) {
        /* Server doesn't have this message yet */
        cache_size = mailbox_cache_size(mailbox, msgno);

        if (cache_size == 0) {
            syslog(LOG_ERR,
                   "upload_messages(): Empty cache entry for msgno %lu",
                   msgno);
            return(IMAP_INTERNAL);
        }
        
        r = mailbox_map_message(mailbox, record->uid, &msg_base, &msg_size);
        
        if (r) {
            syslog(LOG_ERR, "IOERROR: opening message file %lu of %s: %m",
                   record->uid, mailbox->name);
            return(IMAP_IOERROR);
        }

        prot_printf(toserver, " %lu %lu %lu {%lu+}\r\n",
		    record->header_size, record->content_lines,
		    record->cache_version, cache_size);

        prot_write(toserver,
		   (char *)(mailbox->cache_base + record->cache_offset),
		   cache_size);
                    
        prot_printf(toserver, "{%lu+}\r\n", record->size);
        prot_write(toserver, (char *)msg_base, record->size);
        mailbox_unmap_message(mailbox, record->uid, &msg_base, &msg_size);
        sequence++;
    }
    return(r);
}

static int
upload_messages_list(struct mailbox *mailbox, struct sync_msg_list *list)
{
    unsigned long msgno;
    int r = 0;
    struct index_record record;
    struct sync_msg *msg;
    int count = 0;

    if (chdir(mailbox->path)) {
        syslog(LOG_ERR, "Couldn't chdir to %s: %s",
               mailbox->path, strerror(errno));
        return(IMAP_IOERROR);
    }

    msg = list->head;
    for (msgno = 1 ; msgno <= mailbox->exists ; msgno++) {
        r = mailbox_read_index_record(mailbox, msgno, &record);

        if (r) {
            syslog(LOG_ERR,
                   "IOERROR: reading index entry for nsgno %lu of %s: %m",
                   record.uid, mailbox->name);
            return(IMAP_IOERROR);
        }

        /* Skip over messages recorded on server which are missing on client
         * (either will be expunged or have been expunged already) */
        while (msg && (record.uid > msg->uid))
            msg = msg->next;

        if (msg && (record.uid == msg->uid) &&
            message_uuid_compare(&record.uuid, &msg->uuid)) {
            msg = msg->next;  /* Ignore exact match */
            continue;
        }

        if (count++ == 0)
            prot_printf(toserver, "UPLOAD %lu %lu",
                     mailbox->last_uid, mailbox->last_appenddate); 

        /* Message with this UUID exists on client but not server */
        if ((r=upload_message_work(mailbox, msgno, &record)))
            return(r);

        if (msg && (msg->uid == record.uid))  /* Overwritten on server */
            msg = msg->next;
    }

    if (count == 0)
        return(r);

    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);
    return(sync_parse_code("UPLOAD", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

static int
upload_messages_from(struct mailbox *mailbox, unsigned long old_last_uid)
{
    unsigned long msgno;
    int r = 0;
    struct index_record record;
    int count = 0;

    if (chdir(mailbox->path)) {
        syslog(LOG_ERR, "Couldn't chdir to %s: %s",
               mailbox->path, strerror(errno));
        return(IMAP_IOERROR);
    }

    for (msgno = 1 ; msgno <= mailbox->exists ; msgno++) {
        r =  mailbox_read_index_record(mailbox, msgno, &record);

        if (r) {
            syslog(LOG_ERR,
                   "IOERROR: reading index entry for nsgno %lu of %s: %m",
                   record.uid, mailbox->name);
            return(IMAP_IOERROR);
        }

        if (record.uid <= old_last_uid)
            continue;

        if (count++ == 0)
            prot_printf(toserver, "UPLOAD %lu %lu",
                     mailbox->last_uid, mailbox->last_appenddate); 

        if ((r=upload_message_work(mailbox, msgno, &record)))
            return(r);
    }

    if (count == 0)
        return(r);

    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);
    return(sync_parse_code("UPLOAD", fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}

/* upload_messages() null operations still requires UIDLAST update */

static int
update_uidlast(struct mailbox *mailbox)
{
    prot_printf(toserver, "UIDLAST %lu %lu\r\n",
             mailbox->last_uid, mailbox->last_appenddate);
    prot_flush(toserver);
    return(sync_parse_code("UIDLAST",fromserver, SYNC_PARSE_EAT_OKLINE, NULL));
}


/* ====================================================================== */

static int
do_seen_work(struct mailbox *mailbox,
             char *user, time_t *lastseenp, int selected)
{    
    int r = 0;
    struct seen *seendb;
    time_t lastread, lastchange;
    unsigned int last_recent_uid;
    char *seenuid;

    if (!r) r = seen_open(mailbox, user, 0, &seendb);
    if (!r) {
        r = seen_read(seendb, &lastread, &last_recent_uid,
                      &lastchange, &seenuid);
        seen_close(seendb);
    }

    if (r) {
        syslog(LOG_ERR, "Failed to read seendb (%s, %s): %s",
               mailbox->name, user, error_message(r));
        return(r);
    }

    if (lastseenp && (*lastseenp == lastchange))
        return(r);

    if (!selected &&
        ((r = folder_select(mailbox->name, mailbox->uniqueid, NULL, NULL))))
        return(r);

    /* Update seen list */
    prot_printf(toserver, "SETSEEN %s %lu %lu %lu ",
             user, lastread, last_recent_uid, lastchange);
    sync_printastring(toserver, seenuid);
    prot_printf(toserver, "\r\n");
    prot_flush(toserver);
    free(seenuid);
    return(sync_parse_code("SETSEEN",fromserver,SYNC_PARSE_EAT_OKLINE,NULL));
}


static int
do_seen(char *name, char *user)
{
    int r = 0;
    struct mailbox m;

    if (verbose) 
        printf("SEEN %s %s\n", name, user);

    if (verbose_logging)
        syslog(LOG_INFO, "SEEN %s %s", name, user);
    
    sync_authstate = auth_newstate(user);

    if ((r = send_user(user)))
        goto bail;

    if ((r = mailbox_open_header(name, 0, &m)))
        goto bail;

    r = mailbox_open_index(&m);
    if (!r) r = do_seen_work(&m, user, NULL, 0);
    mailbox_close(&m);

 bail:
    send_enduser();
    auth_freestate(sync_authstate);
    return(r);
}

/* ====================================================================== */

static int
do_append(char *name, char *user)
{
    struct mailbox m;
    int r = 0;
    int mailbox_open = 0;
    int selected = 0;
    unsigned long last_uid  = 0;
    time_t last_seen = 0;
    struct index_record record;

    if (verbose) 
        printf("APPEND %s\n", name);

    if (verbose_logging)
        syslog(LOG_INFO, "APPEND %s", name);

    sync_authstate = auth_newstate(user);

    if ((r = send_user(user)))
        goto bail;

    if ((r = mailbox_open_header(name, 0, &m)))
        goto bail;

    mailbox_open = 1;
    
    if ((r = mailbox_open_index(&m)))
        goto bail;

    if ((r = folder_select(name, m.uniqueid, &last_uid, &last_seen)))
        goto bail;

    selected = 1;

    if ((r = mailbox_read_index_record(&m, m.exists, &record)))
        goto bail;

    if ((record.uid > last_uid) && (r=upload_messages_from(&m, last_uid)))
        goto bail;

    /* Append may also update seen state (but only if msgs tagged as \Seen) */
    r = do_seen_work(&m, user, &last_seen, 1);

 bail:
    if (mailbox_open) mailbox_close(&m);
    send_enduser();
    auth_freestate(sync_authstate);
    return(r);
}

/* ====================================================================== */

/* Caller should acquire expire lock before opening mailbox index:
 * gives us readonly snapshot of mailbox for duration of upload
 */

static int
do_mailbox_work(struct mailbox *mailbox, 
                struct sync_msg_list *list, int just_created,
                char *uniqueid, char *seen_user)
{
    unsigned int last_recent_uid;
    time_t lastread, lastchange;
    struct seen *seendb;
    char *seenuid;
    int r = 0;
    int selected = 0;
    int flag_lookup_table[MAX_USER_FLAGS];

    create_flags_lookup(flag_lookup_table,
                        mailbox->flagname, list->meta.flagname);

    if (check_flags(mailbox, list, flag_lookup_table)) {
        if (!selected &&
            (r=folder_select(mailbox->name, mailbox->uniqueid, NULL, NULL)))
            return(r);

        selected = 1;
        if ((r=update_flags(mailbox, list, flag_lookup_table)))
            goto bail;
    }
    
    if (check_expunged(mailbox, list)) {
        if (!selected &&
            (r=folder_select(mailbox->name, mailbox->uniqueid, NULL, NULL)))
            goto bail;

        selected = 1;

        if ((r=expunge(mailbox, list)))
            goto bail;
    }

    if (check_upload_messages(mailbox, list)) {
        if (!selected &&
            (r=folder_select(mailbox->name, mailbox->uniqueid, NULL, NULL)))
            goto bail;
        selected = 1;

        if ((r=upload_messages_list(mailbox, list)))
            goto bail;
    } else if (just_created || (list->last_uid != mailbox->last_uid)) {
        if (!selected &&
            (r=folder_select(mailbox->name, mailbox->uniqueid, NULL, NULL)))
            goto bail;
        selected = 1;

        if ((r=update_uidlast(mailbox)))
            goto bail;
    }

    if (seen_user) {
        r = seen_open(mailbox, seen_user, 0, &seendb);
        if (!r) {
            r = seen_read(seendb, &lastread, &last_recent_uid,
                          &lastchange, &seenuid);
            seen_close(seendb);
        }
        if (r) {
            /* Fake empty seendb entry if no info */
            lastread = last_recent_uid = 0;
            seenuid  = strdup("");
            r = 0;
        }

        if (just_created ||
            (lastchange > list->lastseen) ||
            (last_recent_uid > list->last_recent_uid)) {
            if (!selected) {
                r=folder_select(mailbox->name, mailbox->uniqueid, NULL, NULL);
                if (r) goto bail;
            }
            selected = 1;

            prot_printf(toserver, "SETSEEN %s %lu %lu %lu ",
                     seen_user, lastread, last_recent_uid, lastchange);
            sync_printastring(toserver, seenuid);
            prot_printf(toserver, "\r\n");
            prot_flush(toserver);

            r=sync_parse_code("SETSEEN",fromserver,SYNC_PARSE_EAT_OKLINE,NULL);
        }

        free(seenuid);
    }

 bail:
    return(r);
}

/* ====================================================================== */

int
do_folders(char *user,
           struct sync_folder_list *client_list,
           struct sync_folder_list *server_list,
           int *vanishedp,
           int do_contents)
{
    struct mailbox m;
    int r = 0, mailbox_open = 0;
    struct sync_rename_list *rename_list = sync_rename_list_create();
    struct sync_folder   *folder, *folder2;

    *vanishedp = 0;

    if (do_contents) {
        /* Attempt to reserve messages on server that we would overwise have
         * to upload from client */
        if ((r = reserve_messages(client_list, server_list, vanishedp)))
            goto bail;
    } else {
        /* Just need to check whether folders exist, get uniqueid */
        if ((r = folders_get_uniqueid(client_list, vanishedp)))
            goto bail;
    }

    /* Tag folders on server which still exist on the client. Anything
     * on the server which remains untagged can be deleted immediately */
    for (folder = client_list->head ; folder ; folder = folder->next) {
        if (folder->id &&
            (folder2 = sync_folder_lookup(server_list, folder->id))) {
            folder2->mark = 1;
            if (strcmp(folder->name , folder2->name) != 0)
                sync_rename_list_add(rename_list,
                                     folder->id, folder2->name, folder->name);
        }
    }

    /* Delete folders on server which no longer exist on client */
    for (folder = server_list->head ; folder ; folder = folder->next) {
        if (!folder->mark && ((r=folder_delete(folder->name)) != 0))
            goto bail;
    }

    /* Need to rename folders in an order which avoids dependancy conflicts
     * following isn't wildly efficient, but rename_list will typically be
     * short and contain few dependancies.  Algorithm is to simply pick a
     * rename operation which has no dependancy and repeat until done */

    while (rename_list->done < rename_list->count) {
        int rename_success = 0;
        struct sync_rename_item *item, *item2;

        for (item = rename_list->head; item; item = item->next) {
            if (item->done) continue;

            item2 = sync_rename_lookup(rename_list, item->newname);
            if (item2 && !item2->done) continue;

            /* Found unprocessed item which should rename cleanly */
            if ((r = folder_rename(item->oldname, item->newname))) {
                syslog(LOG_ERR, "do_folders(): failed to rename: %s -> %s ",
                       item->oldname, item->newname);
                goto bail;
            }

            rename_list->done++;
            item->done = 1;
            rename_success = 1;
        }

        if (!rename_success) {
            /* Scanned entire list without a match */
            syslog(LOG_ERR,
                   "do_folders(): failed to order folders correctly");
            r = IMAP_PROTOCOL_ERROR;
            goto bail;
        }
    }

    for (folder = client_list->head ; folder ; folder = folder->next) {
        if (!folder->id) continue;

        r = mailbox_open_header(folder->name, 0, &m);

        /* Deal with folders deleted since start of function call. Likely
         * cause concurrent rename/delete: caller may need countermeaures
         * (e.g: lock out imapds for a few seconds and then retry)
         */
        if (r == IMAP_MAILBOX_NONEXISTENT) {
            (*vanishedp)++;

            folder2 = sync_folder_lookup(server_list, folder->id);

            if (folder2 && folder2->mark) {
                if ((r=folder_delete(folder2->name))) goto bail;
                folder2->mark = 0;
            }
            continue;
        }

        /* Quietly ignore objects that we don't have access to.
         * Includes directory stubs, which have not underlying cyrus.*
         * files in the filesystem */
        if (r == IMAP_PERMISSION_DENIED) {
            r = 0;
            continue;
        }

        if (!r) mailbox_open = 1;
#if 0
        if (!r) r = mailbox_lock_expire(&m);   /* YYY Two phase expunge hook */
#endif
        if (!r) r = mailbox_open_index(&m);

        if (r) {
            if (mailbox_open) mailbox_close(&m);
            syslog(LOG_ERR, "IOERROR: Failed to open %s: %s",
                   folder->name, error_message(r));
            r = IMAP_IOERROR;
            goto bail;
        }

        if ((folder2=sync_folder_lookup(server_list, folder->id))) {
            if (strcmp(folder2->id, m.uniqueid) != 0) {
                /* Folder UniqueID has changed under our feet: force resync */

                if ((r=folder_delete(folder2->name)))
                    goto bail;

                if ((r=folder_create(m.name,m.uniqueid,m.acl,m.uidvalidity)))
                    goto bail;

                if (do_contents) {
                    struct sync_msg_list *folder_msglist;

                    /* 0L, 0L Forces last_uid and seendb push as well */
                    folder_msglist = sync_msg_list_create(m.flagname, 0, 0, 0);
                    r = do_mailbox_work(&m, folder_msglist, 1,
                                        m.uniqueid, user);
                    sync_msg_list_free(&folder_msglist);
                }
            } else {
                /* Deal with existing folder */
                if (!(folder2->acl && !strcmp(m.acl, folder2->acl)))
                    r = folder_setacl(folder->name, m.acl);
                
                if (!r && do_contents)
                    r = do_mailbox_work(&m, folder2->msglist, 0,
                                        m.uniqueid, user);
            }
        } else {
            /* Need to create fresh folder on server */
            if ((r=folder_create(m.name, m.uniqueid, m.acl, m.uidvalidity)))
                goto bail;

            if (do_contents) {
                struct sync_msg_list *folder_msglist;

                /* 0L, 0L Forces last_uid and seendb push as well */
                folder_msglist = sync_msg_list_create(m.flagname, 0, 0, 0);
                r = do_mailbox_work(&m, folder_msglist, 1, m.uniqueid, user);
                sync_msg_list_free(&folder_msglist);
            }
        }
        if (r) goto bail;

        mailbox_close(&m);
        mailbox_open = 0;
    }

 bail:
    if (mailbox_open) mailbox_close(&m);
    sync_rename_list_free(&rename_list);
    return(r);
}

/* ====================================================================== */

/* Generate sync_folder_list including all msg information from
   list of client folders */

int
do_user_some(char *userid,
             struct sync_folder_list *client_list,
             struct sync_folder_list *server_list)
{
    struct sync_folder *folder = NULL;
    int               c = ' ', r = 0;
    int               unsolicited_type;
    struct sync_msg  *msg = NULL;
    static struct buf id;
    static struct buf acl;
    static struct buf name;
    static struct buf lastuid;
    static struct buf last_recent_uid;
    static struct buf lastseen;
    static struct buf arg;

    prot_printf(toserver, "USER_SOME %s", userid); 
    for (folder = client_list->head ; folder; folder = folder->next) {
        prot_printf(toserver, " "); 
        sync_printastring(toserver, folder->name);
    }
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);

    r = sync_parse_code("USER_SOME", fromserver,
                        SYNC_PARSE_EAT_OKLINE, &unsolicited_type);

    while (!r && (unsolicited_type > 0)) {
        switch (unsolicited_type) {
        case 2:
            /* New folder */
            if ((c = getword(fromserver, &id)) != ' ')
                goto parse_err;

            if ((c = getastring(fromserver, toserver, &name)) != ' ')
                goto parse_err;

            if ((c = getastring(fromserver, toserver, &acl)) != ' ')
                goto parse_err;

            if ((c = getastring(fromserver, toserver, &lastuid)) != ' ')
                goto parse_err;

            if ((c = getastring(fromserver, toserver, &lastseen)) != ' ')
                goto parse_err;

            c = getastring(fromserver, toserver, &last_recent_uid);

            if (c == '\r') c = prot_getc(fromserver);
            if (c != '\n') goto parse_err;
            if (!imparse_isnumber(lastuid.s))  goto parse_err;
            if (!imparse_isnumber(lastseen.s)) goto parse_err;
            if (!imparse_isnumber(last_recent_uid.s)) goto parse_err;

            folder = sync_folder_list_add(server_list, id.s, name.s, acl.s);
            folder->msglist
                = sync_msg_list_create(NULL, sync_atoul(lastuid.s),
                                       sync_atoul(lastseen.s),
                                       sync_atoul(last_recent_uid.s));
            break;
        case 1:
            /* New message in current folder */
            if (folder == NULL) goto parse_err;       /* No current folder */
            msg = sync_msg_list_add(folder->msglist);
        
            if (((c = getword(fromserver, &arg)) != ' ') ||
                ((msg->uid = sync_atoul(arg.s)) == 0)) goto parse_err;
            
            if (((c = getword(fromserver, &arg)) != ' ')) goto parse_err;

            if (!message_uuid_from_text(&msg->uuid, arg.s))
                goto parse_err;

            c = sync_getflags(fromserver, &msg->flags, &folder->msglist->meta);
            if (c == '\r') c = prot_getc(fromserver);
            if (c != '\n') goto parse_err;
            break;
        default:
            goto parse_err;
        }
        r = sync_parse_code("USER_SOME", fromserver,
                            SYNC_PARSE_EAT_OKLINE, &unsolicited_type);
    }
    return(r);

 parse_err:
    syslog(LOG_ERR,
           "USER_SOME: Invalid unsolicited response type %d from server: %s",
           unsolicited_type, arg.s);
    sync_eatlines_unsolicited(fromserver, c);
    return(IMAP_PROTOCOL_ERROR);
}

/* ====================================================================== */

static int
do_mailboxes(char *userid, struct sync_folder_list *client_folder_list)
{
    struct sync_folder_list *server_folder_list = sync_folder_list_create();
    int r = 0;
    int vanished = 0;
    struct sync_folder *folder;

    if (verbose) {
        printf("MAILBOXES");

        for (folder = client_folder_list->head; folder ; folder = folder->next)
            printf(" %s", folder->name);

        printf("\n");
    }

    if (verbose_logging) {
        for (folder = client_folder_list->head; folder ; folder = folder->next)
            syslog(LOG_INFO, "MAILBOX %s", folder->name);
    }

    sync_authstate = auth_newstate(userid);

    /* Worthwhile doing user_some even in case of single mailbox:
     * catches duplicate messages in single folder. Only cost is that
     * mailbox at server end is opened twice: once for user_some(),
     * once for do_folders() */

    if (!r) r = do_user_some(userid, client_folder_list, server_folder_list);
    if (!r) r = do_folders(userid, client_folder_list, server_folder_list,
                           &vanished, 1);

    send_enduser();
    auth_freestate(sync_authstate);
    sync_folder_list_free(&server_folder_list);
    return(r);
}

/* ====================================================================== */

static int
addmbox(char *name,
        int matchlen __attribute__((unused)),
        int maycreate __attribute__((unused)),
        void *rock)
{
    struct sync_folder_list *list = (struct sync_folder_list *) rock;

    sync_folder_list_add(list, NULL, name, NULL);
    return(0);
}

static int
addmbox_sub(char *name,
            int matchlen __attribute__((unused)),
            int maycreate __attribute__((unused)),
            void *rock)
{
    struct sync_folder_list *list = (struct sync_folder_list *) rock;

    sync_folder_list_add(list, name, name, NULL);
    return(0);
}

/* ====================================================================== */

int
do_mailbox_preload(struct sync_folder *folder)
{
    struct mailbox m;
    int r = 0;
    unsigned long msgno;
    struct index_record record;
    int lastuid = 0;

    if ((r=mailbox_open_header(folder->name, 0, &m)))
        return(r);

    if (!r) r = mailbox_open_index(&m);

    /* Quietly preload data from index */
    for (msgno = 1 ; msgno <= m.exists; msgno++) {
        mailbox_read_index_record(&m, msgno, &record);

        /* Fairly pointless, just to ensure that compiler doesn't
           optimise loop away somehow */
        if (record.uid <= lastuid)
            syslog(LOG_ERR, "cmd_status_work_sub(): UIDs out of order!");
    }

    mailbox_close(&m);
    return r;
}

int
do_user_preload(char *user)
{
    char buf[MAX_MAILBOX_NAME+1];
    int r = 0;
    struct sync_folder_list *client_list = sync_folder_list_create();
    struct sync_folder *folder;

    /* Generate full list of folders on client size */
    snprintf(buf, sizeof(buf)-1, "user.%s", user);
    addmbox(buf, 0, 0, (void *)client_list);

    snprintf(buf, sizeof(buf)-1, "user.%s.*", user);
    r = (sync_namespace.mboxlist_findall)(&sync_namespace, buf, 0,
                                          user, sync_authstate, addmbox,
                                          (void *)client_list);

    if (r) {
        syslog(LOG_ERR, "IOERROR: %s", error_message(r));
        sync_folder_list_free(&client_list);
        return(r);
    }

    for (folder = client_list->head ; folder ; folder = folder->next) {
        r = do_mailbox_preload(folder);

        if (r) break;
    }

    sync_folder_list_free(&client_list);
    return(r);
}

int
do_user_main(char *user, struct sync_folder_list *server_list, int *vanishedp)
{
    char buf[MAX_MAILBOX_NAME+1];
    int r = 0;
    struct sync_folder_list *client_list = sync_folder_list_create();

    /* Generate full list of folders on client size */
    snprintf(buf, sizeof(buf)-1, "user.%s", user);
    addmbox(buf, 0, 0, (void *)client_list);

    snprintf(buf, sizeof(buf)-1, "user.%s.*", user);
    r = (sync_namespace.mboxlist_findall)(&sync_namespace, buf, 0,
                                          user, sync_authstate, addmbox,
                                          (void *)client_list);

    if (r) {
        syslog(LOG_ERR, "IOERROR: %s", error_message(r));
        return(r);
    }

    return(do_folders(user, client_list, server_list, vanishedp, 1));
}

int
do_user_sub(char *user, struct sync_folder_list *server_list)
{
    char buf[MAX_MAILBOX_NAME+1];
    int r = 0;
    struct sync_folder_list *client_list = sync_folder_list_create();
    struct sync_folder *folder, *folder2;

    /* Includes subsiduary nodes automatically */
    snprintf(buf, sizeof(buf)-1, "user.%s", user);
    r = (sync_namespace.mboxlist_findsub)(&sync_namespace, buf, 0,
                                          user, sync_authstate, addmbox_sub,
                                          (void *)client_list, 0);
    if (r) {
        syslog(LOG_ERR, "IOERROR: %s", error_message(r));
        goto bail;
    }

    for (folder = client_list->head ; folder ; folder = folder->next) {
        if ((folder2 = sync_folder_lookup(server_list, folder->name)))
            folder2->mark = 1;
        else if ((r=folder_addsub(folder->name)))
            goto bail;
    }

    for (folder = server_list->head ; folder ; folder = folder->next) {
        if (!folder->mark && ((r=folder_delsub(folder->name))))
            goto bail;
    }

 bail:
    sync_folder_list_free(&client_list);
    return(r);
}

int
do_user_sieve(char *user, struct sync_sieve_list *server_list)
{
    int r = 0;
    struct sync_sieve_list *client_list;
    struct sync_sieve_item *item, *item2;
    int client_active = 0;
    int server_active = 0;

    if ((client_list = sync_sieve_list_generate(user)) == NULL) {
        syslog(LOG_ERR, "Unable to list sieve scripts for %s", user);
        return(IMAP_IOERROR);
    }

    /* Upload missing and out of date scripts */
    for (item = client_list->head ; item ; item = item->next) {
        if ((item2 = sync_sieve_lookup(server_list, item->name))) {
            item2->mark = 1;
            if ((item2->last_update < item->last_update) &&
                (r=sieve_upload(user, item->name, item->last_update)))
                goto bail;
        } else if ((r=sieve_upload(user, item->name, item->last_update)))
            goto bail;
    }

    /* Delete scripts which no longer exist on the client */
    server_active = 0;
    for (item = server_list->head ; item ; item = item->next) {
        if (item->mark) {
            if (item->active) server_active = 1;
        } else if ((r=sieve_delete(item->name)))
            goto bail;
    }

    /* Change active script if necassary */
    client_active = 0;
    for (item = client_list->head ; item ; item = item->next) {
        if (!item->active)
            continue;

        client_active = 1;
        if (!((item2 = sync_sieve_lookup(server_list, item->name)) &&
              (item2->active))) {
            if ((r = sieve_activate(item->name)))
                goto bail;

            server_active = 1;
        }
        break;
    }

    if (!r && !client_active && server_active)
        r = sieve_deactivate();

 bail:
    sync_sieve_list_free(&client_list);
    return(r);
}

/* do_user_all() separated into two parts so that we can start process
 * asynchronously, come back and parse result when local list generated */

int
do_user_all_start(char *user)
{
    prot_printf(toserver, "USER_ALL "); 
    sync_printastring(toserver, user);
    prot_printf(toserver, "\r\n"); 
    prot_flush(toserver);
    return(0);
}

int
do_user_all_parse(char *user,
                  struct sync_folder_list *server_list,
                  struct sync_folder_list *server_sub_list,
                  struct sync_sieve_list  *server_sieve_list,
                  struct quota *quota)
{
    int r = 0;
    int c = ' ';
    int active = 0;
    int unsolicited_type;
    static struct buf id;
    static struct buf name;
    static struct buf time;
    static struct buf flag;
    static struct buf acl;
    static struct buf lastuid;
    static struct buf lastseen;
    static struct buf last_recent_uid;
    static struct buf arg;
    struct sync_folder *folder = NULL;
    struct sync_msg    *msg    = NULL;

    r = sync_parse_code("USER_ALL", fromserver,
                        SYNC_PARSE_NOEAT_OKLINE, &unsolicited_type);

    /* Unpleasant: translate remote access error into "please reset me" */
    if (r == IMAP_MAILBOX_NONEXISTENT)
        return(0);

    while (!r && (unsolicited_type > 0)) {
        switch (unsolicited_type) {
        case 4:
            /* New Sieve script */
            c = getastring(fromserver, toserver, &name);
            if (c != ' ') goto parse_err;
            c = getastring(fromserver, toserver, &time);
            if (c == ' ') {
                c = getastring(fromserver, toserver, &flag);
                if (!strcmp(flag.s, "*"))
                    active = 1;
            } else
                active = 0;

            if (c == '\r') c = prot_getc(fromserver);
            if (c != '\n') goto parse_err;
            sync_sieve_list_add(server_sieve_list,
                                name.s, atoi(time.s), active);
            break;
        case 3:
            /* New subscription */
            c = getastring(fromserver, toserver, &name);
            if (c == '\r') c = prot_getc(fromserver);
            if (c != '\n') goto parse_err;
            sync_folder_list_add(server_sub_list, name.s, name.s, NULL);
            break;
        case 2:
            /* New folder */
            if ((c = getword(fromserver, &id)) != ' ')
                goto parse_err;
        
            if ((c = getastring(fromserver, toserver, &name)) != ' ')
                goto parse_err;

            if ((c = getastring(fromserver, toserver, &acl)) != ' ')
                goto parse_err;

            if ((c = getastring(fromserver, toserver, &lastuid)) != ' ')
                goto parse_err;

            if ((c = getastring(fromserver, toserver, &lastseen)) != ' ')
                goto parse_err;

            c = getastring(fromserver, toserver, &last_recent_uid);

            if (c == '\r') c = prot_getc(fromserver);
            if (c != '\n') goto parse_err;
            if (!imparse_isnumber(lastuid.s)) goto parse_err;
            if (!imparse_isnumber(lastseen.s)) goto parse_err;
            if (!imparse_isnumber(last_recent_uid.s)) goto parse_err;

            folder = sync_folder_list_add(server_list, id.s, name.s, acl.s);
            folder->msglist
                = sync_msg_list_create(NULL, sync_atoul(lastuid.s),
                                       sync_atoul(lastseen.s),
                                       sync_atoul(last_recent_uid.s));
            break;
        case 1:
            /* New message in current folder */
            if (folder == NULL) goto parse_err;       /* No current folder */
            msg = sync_msg_list_add(folder->msglist);

            if (((c = getword(fromserver, &arg)) != ' ') ||
                ((msg->uid = sync_atoul(arg.s)) == 0)) goto parse_err;
            
            if (((c = getword(fromserver, &arg)) != ' ')) goto parse_err;

            if (!message_uuid_from_text(&msg->uuid, arg.s))
                goto parse_err;

            c = sync_getflags(fromserver, &msg->flags, &folder->msglist->meta);
            if (c == '\r') c = prot_getc(fromserver);
            if (c != '\n') goto parse_err;
            break;
        default:
            goto parse_err;
        }

        r = sync_parse_code("USER_ALL", fromserver,
                            SYNC_PARSE_NOEAT_OKLINE, &unsolicited_type);
    }
    if (r) return(r);

    /* Parse quota response */

    c = getword(fromserver, &arg);
    quota->limit = atoi(arg.s);

    if (c == '\r') c = prot_getc(fromserver);
    if (c != '\n') goto parse_err;

    return(0);

 parse_err:
    syslog(LOG_ERR, "USER_ALL: Invalid type %d response from server",
           unsolicited_type);
    sync_eatlines_unsolicited(fromserver, c);
    return(IMAP_PROTOCOL_ERROR);
}

int
do_user_work(char *user, int *vanishedp)
{
    char buf[MAX_MAILBOX_NAME+1];
    int r = 0, mailbox_open = 0;
    struct sync_folder_list *server_list      = sync_folder_list_create();
    struct sync_folder_list *server_sub_list  = sync_folder_list_create();
    struct sync_sieve_list *server_sieve_list = sync_sieve_list_create();
    struct quota server_quota;
    struct mailbox m;
    struct sync_folder *folder2;

    if (verbose) 
        printf("USER %s\n", user);

    if (verbose_logging)
        syslog(LOG_INFO, "USER %s", user);

    memset(&server_quota, 0, sizeof(struct quota));

    sync_authstate = auth_newstate(user);

    /* Get server started */
    do_user_all_start(user);

    /* Preload data at client end while server is working */
    do_user_preload(user);

    r = do_user_all_parse(user,
                          server_list, server_sub_list, server_sieve_list,
                          &server_quota);

    if (r) {
        sync_folder_list_free(&server_list);
        sync_folder_list_free(&server_sub_list);
        return(r);
    }

    snprintf(buf, sizeof(buf)-1, "user.%s", user);
    r = mailbox_open_header(buf, 0, &m);
    if (!r) mailbox_open = 1;
    if (!r) r = mailbox_open_index(&m);

    if (r) {
        if (mailbox_open) mailbox_close(&m);
        syslog(LOG_ERR, "IOERROR: Failed to open %s: %s",
               m.name, error_message(r));
        r = IMAP_IOERROR;
        goto bail;
    }

    /* Reset target account entirely if uniqueid of inbox doesn't match
     * (Most likely reason is that source account has been replaced)
     * Also if mailbox doesn't exist at all on target.
     */
    if (((folder2 = sync_folder_lookup_byname(server_list, m.name)) == NULL) ||
        (strcmp(m.uniqueid, folder2->id) != 0)) {
        r = user_reset(user);

        /* Reset local copies */
        sync_folder_list_free(&server_list);
        sync_folder_list_free(&server_sub_list);
        server_list     = sync_folder_list_create();
        server_sub_list = sync_folder_list_create();
        memset(&server_quota, 0, sizeof(struct quota));
    }

    /* Update/Create quota */
    if (!r) r = update_quota_work(user, &m, &server_quota);
    mailbox_close(&m);

    if (!r) r = do_user_main(user, server_list, vanishedp);
    if (!r) r = do_user_sub(user, server_sub_list);
    if (!r) r = do_user_sieve(user, server_sieve_list);

 bail:
    send_enduser();
    auth_freestate(sync_authstate);
    sync_folder_list_free(&server_list);
    sync_folder_list_free(&server_sub_list);
    sync_sieve_list_free(&server_sieve_list);
    return(r);
}

int
do_user(char *user)
{
    struct sync_user_lock user_lock;
    int r = 0;
    int vanished = 0;

    /* Most of the time we don't need locking here: rename (the only
     * complicated case) is pretty rare, especially in the middle of the
     * night, which is when most of this will be going on */
    r = do_user_work(user, &vanished);

    /* Complication: monthly folder rotation causes rapid rename+create.
     *
     * mailbox_open_header() and mailbox_open_index() bail out with
     * IMAP_MAILBOX_BADFORMAT if they try to open a mailbox which is
     * currently in the process of being created. This is a nasty race
     * condition which imapd just ignores (presumably on the principle that
     * rapid rename+create+select would be very rare in normal use).
     *
     * We could solve this problem by putting a sync_user_lock() around
     * _every_ single replication operation, but this is tedious and would
     * probably involve quite a lot of overhead. As an experiment
     * catch IMAP_MAILBOX_BADFORMAT and have another go while locking out
     * user access to the mboxlist.
     */

    if (r == IMAP_MAILBOX_BADFORMAT)
        syslog(LOG_ERR,
               "do_user() IMAP_MAILBOX_BADFORMAT: retrying with snapshot");

    if ((r == IMAP_MAILBOX_BADFORMAT) || (vanished > 0)) {
        /* (vanished > 0): If we lost a folder in transit, lock the user
         * out of mboxlist for a few seconds while we retry. Will be a NOOP
         * if folder actually was deleted during do_user_work run.
         * Following just protects us against folder rename smack in the
         * middle of night or manual sys. admin inspired sync run */

        sync_user_lock_reset(&user_lock);
        sync_user_lock(&user_lock, user);
        r = do_user_work(user, &vanished);
        sync_user_unlock(&user_lock);
    }
    return(r);
}

/* ====================================================================== */

int
do_meta_sub(char *user)
{
    int unsolicited, c, r = 0;
    static struct buf name;
    struct sync_folder_list *server_list = sync_folder_list_create();

    prot_printf(toserver, "LSUB\r\n"); 
    prot_flush(toserver);
    r=sync_parse_code("LSUB",fromserver, SYNC_PARSE_EAT_OKLINE, &unsolicited);

    while (!r && unsolicited) {
        c = getastring(fromserver, toserver, &name);

        if (c == '\r') c = prot_getc(fromserver);
        if (c != '\n') {
            syslog(LOG_ERR, "LSUB: Invalid type %d response from server: %s",
                   unsolicited, name.s);
            sync_eatlines_unsolicited(fromserver, c);
            r = IMAP_PROTOCOL_ERROR;
            break;
        }
        sync_folder_list_add(server_list, name.s, name.s, NULL);

        r = sync_parse_code("LSUB", fromserver,
                            SYNC_PARSE_EAT_OKLINE, &unsolicited);
    }

    if (!r) r = do_user_sub(user, server_list);

    sync_folder_list_free(&server_list);
    return(r);
}

static int
do_meta_quota(char *user)
{
    char buf[MAX_MAILBOX_NAME+1];
    static struct buf token;
    int c, r = 0, mailbox_open = 0;
    struct quota quota;
    struct mailbox m;

    snprintf(buf, sizeof(buf)-1, "user.%s", user);
    r = mailbox_open_header(buf, 0, &m);
    if (!r) mailbox_open = 1;
    if (!r) r = mailbox_open_index(&m);

    if (r) {
        if (mailbox_open) mailbox_close(&m);
        syslog(LOG_ERR, "IOERROR: Failed to open %s: %s",
               m.name, error_message(r));
        r = IMAP_IOERROR;
        goto fail;
    }

    sprintf(buf, "user.%s", user);
    prot_printf(toserver, "QUOTA ");
    sync_printastring(toserver, buf);
    prot_printf(toserver, "\r\n");
    prot_flush(toserver);
    if ((r=sync_parse_code("QUOTA",fromserver,SYNC_PARSE_NOEAT_OKLINE,NULL)))
        return(r);

    c = getword(fromserver, &token);
    quota.limit = atoi(token.s);

    if (c == '\r') c = prot_getc(fromserver);
    if (c != '\n') goto parse_err;

    r = update_quota_work(user, &m, &quota);
    mailbox_close(&m);
    return(r);

 parse_err:
    syslog(LOG_ERR, "Invalid response to QUOTA command: %s", token.s);
    eatline(fromserver, c);
    r = IMAP_PROTOCOL_ERROR;

 fail:
    if (mailbox_open) mailbox_close(&m);
    return(r);
}

int
do_meta_sieve(char *user)
{
    int unsolicited, c, r = 0;
    static struct buf name;
    static struct buf time;
    static struct buf flag;
    struct sync_sieve_list *server_list = sync_sieve_list_create();
    int active = 0;

    prot_printf(toserver, "LIST_SIEVE\r\n"); 
    prot_flush(toserver);
    r=sync_parse_code("LIST_SIEVE", 
                      fromserver, SYNC_PARSE_EAT_OKLINE, &unsolicited);

    while (!r && unsolicited) {
        c = getastring(fromserver, toserver, &name);

        if (c != ' ') {
            syslog(LOG_ERR,
                   "LIST_SIEVE: Invalid name response from server: %s",
                   name.s);
            sync_eatlines_unsolicited(fromserver, c);
            r = IMAP_PROTOCOL_ERROR;
            break;
        }
        c = getastring(fromserver, toserver, &time);

        if (c == ' ') {
            c = getastring(fromserver, toserver, &flag);
            if (!strcmp(flag.s, "*"))
                active = 1;
        } else
            active = 0;

        if (c == '\r') c = prot_getc(fromserver);
        if (c != '\n') {
            syslog(LOG_ERR,
                   "LIST_SIEVE: Invalid flag response from server: %s",
                   flag.s);
            sync_eatlines_unsolicited(fromserver, c);
            r = IMAP_PROTOCOL_ERROR;
            break;
        }
        sync_sieve_list_add(server_list, name.s, atoi(time.s), active);

        r = sync_parse_code("LIST_SIEVE", fromserver,
                            SYNC_PARSE_EAT_OKLINE, &unsolicited);
    }
    if (r) {
        sync_sieve_list_free(&server_list);
        return(IMAP_IOERROR);
    }

    r = do_user_sieve(user, server_list);

    sync_sieve_list_free(&server_list);
    return(r);
}

static int
do_sieve(char *user)   
{
    int r = 0;

    if (sync_authstate) auth_freestate(sync_authstate);
    sync_authstate = auth_newstate(user);

    if ((r = send_user(user)))
        goto bail;

    r = do_meta_sieve(user);

 bail:
    send_enduser();
    auth_freestate(sync_authstate);
    return(r);
}

int
do_meta(char *user)   
{
    int r = 0;

    if (verbose)
        printf("META %s\n", user);

    if (verbose_logging)
        syslog(LOG_INFO, "META %s", user);

    sync_authstate = auth_newstate(user);

    if ((r = send_user(user)))
        goto bail;

    if (!r) r = do_meta_sub(user);
    if (!r) r = do_meta_quota(user);
    if (!r) r = do_meta_sieve(user);

 bail:
    send_enduser();
    auth_freestate(sync_authstate);
    return(r);
}

/* ====================================================================== */

/*
 * Optimise out redundant clauses:
 *    user  <username> overrides meta   <username>
 *    user  <username> overrides folder user.<username>[.<anything>]
 *    user  <username> overrides append user.<username>[.<anything>]
 *    user  <username> overrides seen   <anything> <user>
 *
 * folder <foldername> <username>  overrides seen <foldername> <username> 
 */

/* XXX Replace params with single structure? */

static void
remove_small(char *user,
             struct sync_action_list *meta_list,
             struct sync_action_list *folder_list,
             struct sync_action_list *append_list,
             struct sync_action_list *seen_list)
{
    struct sync_action *action;
    int len= (user) ? strlen(user) : 0;

    /* user <username> overrides meta <username> */
    for (action = meta_list->head ; action ; action = action->next) {
        if (!strcmp(user, action->name))
            action->active = 0;
    }

    /* user <username> overrides folder user.<username>[.anything] */
    for (action = folder_list->head ; action ; action = action->next) {
        if (!strncmp(action->name, "user.", 5) &&
            !strncmp(user, action->name+5, len) &&
            ((action->name[5+len]=='\0') || (action->name[5+len]=='.')))
            action->active = 0;
    }

    /* user  <username> overrides append <anything> <user> */
    for (action = append_list->head ; action ; action = action->next) {
        if (!strncmp(action->name, "user.", 5) &&
            !strncmp(user, action->name+5, len) &&
            ((action->name[5+len]=='\0') || (action->name[5+len]=='.')))
            action->active = 0;
    }

    /* user  <username> overrides seen   <anything> <user> */
    for (action = seen_list->head ; action ; action = action->next) {
        if (action->seenuser && !strcmp(user, action->seenuser))
            action->active = 0;
    }
}

static void
remove_seen(char *name, char *seenuser, struct sync_action_list *seen_list)
{
    struct sync_action *action;

    for (action = seen_list->head ; action ; action = action->next) {
        if (action->seenuser && seenuser) {
            if (!strcmp(name, action->name) &&
                !strcmp(seenuser, action->seenuser)) {
                action->active = 0;
            }
        } else if (!strcmp(name, action->name)) {
            action->active = 0;
        }
    }
}

static void
remove_append(char *name, struct sync_action_list *append_list)
{
    struct sync_action *action;

    for (action = append_list->head ; action ; action = action->next) {
        if (!strcmp(name, action->name)) {
            action->active = 0;
        }
    }
}

/* ====================================================================== */

char *
do_sync_getuserid(char *s)
{
    static char result[64];
    char *t;
    int len;

    if (strncmp(s, "user.", 5) != 0)
        return(NULL);

    s += 5;
    len = ((t = strchr(s, '.'))) ? (t-s) : strlen(s);

    if ((len+1) > sizeof(result))
        return(NULL);

    memcpy(result, s, len);
    result[len] = '\0';

    return(result);
}

int
do_sync(const char *filename)
{
    struct sync_user_list   *user_folder_list = sync_user_list_create();
    struct sync_user        *user;
    struct sync_action_list *user_list   = sync_action_list_create();
    struct sync_action_list *meta_list   = sync_action_list_create();
    struct sync_action_list *folder_list = sync_action_list_create();
    struct sync_action_list *append_list = sync_action_list_create();
    struct sync_action_list *seen_list   = sync_action_list_create();
    static struct buf type, arg1, arg2;
    char *arg1s, *arg2s;
    char *userid;
    struct sync_action *action;
    int c;
    int fd;
    struct protstream *input;
    int r = 0;

    if ((filename == NULL) || !strcmp(filename, "-"))
        fd = 0;
    else {
        if ((fd = open(filename, O_RDWR)) < 0) {
            syslog(LOG_ERR, "Failed to open %s: %m", filename);
            return(IMAP_IOERROR);
        }

        if (lock_blocking(fd) < 0) {
            syslog(LOG_ERR, "Failed to lock %s: %m", filename);
            return(IMAP_IOERROR);
        }
    }

    input = prot_new(fd, 0);

    while (1) {
	if ((c = getword(input, &type)) == EOF)
            break;

        /* Ignore blank lines */
        if (c == '\r') c = prot_getc(input);
        if (c == '\n')
            continue;

        if (c != ' ') {
            syslog(LOG_ERR, "Invalid input");
            eatline(input, c);
            continue;
        }

	if ((c = getastring(input, 0, &arg1)) == EOF)
            break;
        arg1s = arg1.s;

        if (c == ' ') {
            if ((c = getastring(input, 0, &arg2)) == EOF)
                break;
            arg2s = arg2.s;

        } else 
            arg2s = NULL;
        
        if (c == '\r') c = prot_getc(input);
        if (c != '\n') {
            syslog(LOG_ERR, "Garbage at end of input line");
            eatline(input, c);
            continue;
        }

        ucase(type.s);

        if (!strcmp(type.s, "USER"))
            sync_action_list_add(user_list, arg1s, NULL);
        else if (!strcmp(type.s, "META"))
            sync_action_list_add(meta_list, arg1s, NULL);
        else if (!strcmp(type.s, "MAILBOX"))
            sync_action_list_add(folder_list, arg1s, NULL);
        else if (!strcmp(type.s, "APPEND"))
            sync_action_list_add(append_list, arg1s, NULL);
        else if (!strcmp(type.s, "SEEN"))
            sync_action_list_add(seen_list, arg1s, arg2s);
        else
            syslog(LOG_ERR, "Unknown action type: %s", type.s);
    }

    /* Optimise out redundant clauses */

    for (action = user_list->head ; action ; action = action->next)
        remove_small(action->name,
                     meta_list, folder_list, append_list, seen_list);
    
    for (action = folder_list->head ; action ; action = action->next)
        remove_seen(action->name, action->seenuser, seen_list);

    for (action = folder_list->head ; action ; action = action->next)
        remove_append(action->name, append_list);

    /* And then run tasks. Folder mismatch => fall through to
     * do_user to try and clean things up */

    for (action = append_list->head ; action ; action = action->next) {
        if (!action->active)
            continue;

        if (!(userid = do_sync_getuserid(action->name))) {
            syslog(LOG_ERR, "Ignoring invalid mailbox name: %s", action->name);
            continue;
        }

        if (do_append(action->name, userid)) {
            remove_seen(action->name, action->seenuser, seen_list);
            sync_action_list_add(folder_list, action->name, NULL);
            if (verbose) {
                printf("  Promoting: APPEND %s -> MAILBOX %s\n",
                       action->name, action->name);
            }
            if (verbose_logging) {
                syslog(LOG_INFO, "  Promoting: APPEND %s -> MAILBOX %s",
                       action->name, action->name);
            }
        }
    }

    for (action = seen_list->head ; action ; action = action->next) {
        if (action->active && do_seen(action->name, action->seenuser)) {
            sync_action_list_add(folder_list, action->name, NULL);
            if (verbose) {
                printf("  Promoting: SEEN %s %s -> MAILBOX %s\n",
                       action->name, action->seenuser, action->name);
            }
            if (verbose_logging) {
                syslog(LOG_INFO, "  Promoting: SEEN %s %s -> MAILBOX %s",
                       action->name, action->seenuser, action->name);
            }
        }
    }

    /* Group folder actions by userid for do_mailboxes() */
    for (action = folder_list->head ; action ; action = action->next) {
        if (!action->active)
            continue;

        if (!(userid = do_sync_getuserid(action->name))) {
            syslog(LOG_ERR, "Ignoring invalid mailbox name: %s", action->name);
            continue;
        }

        if (!(user=sync_user_list_lookup(user_folder_list, userid)))
            user = sync_user_list_add(user_folder_list, userid);
            
        if (!sync_folder_lookup_byname(user->folder_list, action->name))
            sync_folder_list_add(user->folder_list, NULL, action->name, NULL);
    }

    for (user = user_folder_list->head ; user ; user = user->next) {
        if ((r=do_mailboxes(user->userid, user->folder_list))) {
            if (r == IMAP_INVALID_USER)
                goto bail;

            remove_small(user->userid,
                         meta_list, folder_list, append_list, seen_list);
            sync_action_list_add(user_list, user->userid, NULL);
            if (verbose) {
                printf("  Promoting: MAILBOXES [%lu] -> USER %s\n",
                       user->folder_list->count, user->userid);
            }
            if (verbose_logging) {
                syslog(LOG_INFO, "  Promoting: MAILBOXES [%lu] -> USER %s",
                       user->folder_list->count, user->userid);
            }
        }
    }

    for (action = meta_list->head ; action ; action = action->next) {
        if (action->active && (r=do_meta(action->name))) {
            if (r == IMAP_INVALID_USER)
                goto bail;

            remove_small(action->name,
                         meta_list, folder_list, append_list, seen_list);
            sync_action_list_add(user_list, action->name, NULL);
            if (verbose) {
                printf("  Promoting: META %s -> USER %s\n",
                       action->name, action->name);
            }
            if (verbose_logging) {
                syslog(LOG_INFO, "  Promoting: META %s -> USER %s",
                       action->name, action->name);
            }
        }
    }

    for (action = user_list->head ; action ; action = action->next) {
        if ((r=do_user(action->name)))
            goto bail;
    }

    sync_user_list_free(&user_folder_list);
    prot_free(input);
    return(0);

 bail:
    if (verbose)
        fprintf(stderr, "Error in do_sync(): bailing out!\n");

    syslog(LOG_ERR, "Error in do_sync(): bailing out!");

    sync_user_list_free(&user_folder_list);
    prot_free(input);
    return(r);
}

/* ====================================================================== */

int
do_daemon_work(const char *sync_log_file, const char *sync_shutdown_file,
               unsigned long timeout, unsigned long min_delta,
               int *restartp)
{
    int r = 0;
    char *work_file_name;
    time_t session_start;
    time_t single_start;
    int    delta;
    struct stat sbuf;

    *restartp = 0;

    work_file_name = xmalloc(strlen(sync_log_file)+20);
    snprintf(work_file_name, strlen(sync_log_file)+20,
             "%s-%d", sync_log_file, getpid());

    session_start = time(NULL);

    while (1) {
        single_start = time(NULL);

        if (sync_shutdown_file && !stat(sync_shutdown_file, &sbuf)) {
            unlink(sync_shutdown_file);
            break;
        }

        if ((timeout > 0) && ((single_start - session_start) > timeout)) {
            *restartp = 1;
            break;
        }

        if (stat(sync_log_file, &sbuf) < 0) {
            if (min_delta > 0) {
                sleep(min_delta);
            } else {
                usleep(100000);    /* 1/10th second */
            }
            continue;
        }

        if (rename(sync_log_file, work_file_name) < 0) {
            syslog(LOG_ERR, "Rename %s -> %s failed: %m",
                   sync_log_file, work_file_name);
            exit(1);
        }

        if ((r=do_sync(work_file_name)))
            return(r);
        
        if (unlink(work_file_name) < 0) {
            syslog(LOG_ERR, "Unlink %s failed: %m", work_file_name);
            exit(1);
        }
        delta = time(NULL) - single_start;

        if ((delta < min_delta) && ((min_delta-delta) > 0))
            sleep(min_delta-delta);
    }
    free(work_file_name);

    if (*restartp == 0)
        return(0);

    prot_printf(toserver, "RESTART\r\n"); 
    prot_flush(toserver);

    r = sync_parse_code("RESTART", fromserver, SYNC_PARSE_EAT_OKLINE, NULL);

    if (r)
        syslog(LOG_ERR, "sync_client RESTART failed");
    else
        syslog(LOG_INFO, "sync_client RESTART succeeded");

    return(r);
}

void
do_daemon(const char *sync_log_file, const char *sync_shutdown_file,
          unsigned long timeout, unsigned long min_delta)
{
    int r = 0;
    pid_t pid;
    int status;
    int restart;

    if (timeout == 0) {
        do_daemon_work(sync_log_file, sync_shutdown_file,
                       timeout, min_delta, &restart);
        return;
    }

    do {
        if ((pid=fork()) < 0)
            fatal("fork failed", EC_SOFTWARE);

        if (pid == 0) {
            r = do_daemon_work(sync_log_file, sync_shutdown_file,
                               timeout, min_delta, &restart);

            if (r)       _exit(1);
            if (restart) _exit(EX_TEMPFAIL);
            _exit(0);
        }
        if (waitpid(pid, &status, 0) < 0)
            fatal("waitpid failed", EC_SOFTWARE);
    } while (WIFEXITED(status) && (WEXITSTATUS(status) == EX_TEMPFAIL));
}

/* ====================================================================== */

static struct sasl_callback mysasl_cb[] = {
    { SASL_CB_GETOPT, &mysasl_config, NULL },
    { SASL_CB_CANON_USER, &mysasl_canon_user, NULL },
    { SASL_CB_LIST_END, NULL, NULL }
};

int
main(int argc, char **argv)
{
    int   opt, i = 0;
    char *alt_config     = NULL;
    char *input_filename = NULL;
    char *servername     = NULL;
    int   r = 0;
    int   exit_rc = 0;
    int   interact = 0;
    int   repeat   = 0;
    int   user     = 0;
    int   mailbox  = 0;
    int   sieve    = 0;
    int   wait     = 0;
    int   timeout  = 600;
    int   min_delta = 0;
    const char *sync_log_file = NULL;
    const char *sync_shutdown_file = NULL;
    char buf[512];
    FILE *file;
    int len;
    struct backend *be = NULL;
    sasl_callback_t *cb;

    /* Global list */
    msgid_onserver = sync_msgid_list_create(SYNC_MSGID_LIST_HASH_SIZE);

    if(geteuid() == 0)
        fatal("must run as the Cyrus user", EC_USAGE);

    setbuf(stdout, NULL);

    while ((opt = getopt(argc, argv, "C:f:vlS:F:w:t:d:Frums")) != EOF) {
        switch (opt) {
        case 'C': /* alt config file */
            alt_config = optarg;
            break;

        case 'v': /* verbose */
            verbose++;
            break;

        case 'l': /* verbose Logging */
            verbose_logging++;
            break;

	case 'S': /* Socket descriptor for server */
	    servername = optarg;
	    break;

        case 'F': /* Shutdown file */
            sync_shutdown_file = optarg;
            break;

        case 'f': /* Input file (sync_log_file used by rolling replication,
                   * input_filename used by user and mailbox modes). */
            sync_log_file = input_filename = optarg;
            break;

        case 'w':
            wait = atoi(optarg);
            break;

        case 't':
            timeout = atoi(optarg);
            break;

        case 'd':
            min_delta = atoi(optarg);
            break;

        case 'r':
            repeat = 1;
            break;

        case 'R': /* alt input file for rolling replication */
            sync_log_file = optarg;
            break;

        case 'u':
            user = 1;
            break;

        case 'm':
            mailbox = 1;
            break;

        case 's':
            sieve = 1;
            break;

        default:
            usage("sync_client");
        }
    }

    /* last arg is server name */
    if (!servername) {
        fprintf(stderr, "No server name specified\n");
        exit(1);
    }

    if ((interact && (            repeat || user || mailbox || sieve)) ||
        (repeat   && (interact           || user || mailbox || sieve)) ||
        (user     && (interact || repeat         || mailbox || sieve)) ||
        (mailbox  && (interact || repeat || user)) ||
        (sieve    && (interact || repeat || user)))
        fatal("Mutially exclusive options defined", EC_USAGE);

    cyrus_init(alt_config, "sync_client", 0);

    if (!config_getstring(IMAPOPT_SYNC_DIR))
        fatal("sync_dir not defined", EC_SOFTWARE);

    if (!config_getstring(IMAPOPT_SYNC_LOG_FILE))
        fatal("sync_log_file not defined", EC_SOFTWARE);

    if (!sync_shutdown_file)
        sync_shutdown_file = config_getstring(IMAPOPT_SYNC_SHUTDOWN_FILE);

    if (!min_delta)
        min_delta = config_getint(IMAPOPT_SYNC_REPEAT_INTERVAL);

    if (!sync_log_file &&
        !(sync_log_file = config_getstring(IMAPOPT_SYNC_LOG_FILE)))
        fatal("sync_log_file not defined", EC_CONFIG);

    /* Just to help with debugging, so we have time to attach debugger */
    if (wait > 0) {
        fprintf(stderr, "Waiting for %d seconds for gdb attach...\n", wait);
        sleep(wait);
    }

    /* Set namespace -- force standard (internal) */
    if ((r = mboxname_init_namespace(&sync_namespace, 1)) != 0) {
        fatal(error_message(r), EC_CONFIG);
    }

    /* open the mboxlist, we'll need it for real work */
    mboxlist_init(0);
    mboxlist_open(NULL);
    mailbox_initialize();

    /* open the quota db, we'll need it for real work */
    quotadb_init(0);
    quotadb_open(NULL);

    signals_set_shutdown(&shut_down);
    signals_add_handlers(0);

    /* load the SASL plugins */
    global_sasl_init(1, 0, mysasl_cb);

    cb = mysasl_callbacks(NULL,
			  config_getstring(IMAPOPT_SYNC_AUTHNAME),
			  config_getstring(IMAPOPT_SYNC_REALM),
			  config_getstring(IMAPOPT_SYNC_PASSWORD));

    /* Open up connection to server */
    be = backend_connect(NULL, servername, &protocol[PROTOCOL_CSYNC],
			 "", cb, NULL);
    if (!be) {
        fprintf(stderr, "Can not connect to server '%s'\n", servername);
        exit(1);
    }

    /* XXX  hack.  should just pass 'be' around */
    fromserver = be->in;
    toserver = be->out;

    if (user) {
        if (input_filename) {
            if ((file=fopen(input_filename, "r")) == NULL) {
                syslog(LOG_NOTICE, "Unable to open %s: %m", input_filename);
                shut_down(1);
            }
            while (fgets(buf, sizeof(buf), file)) {
                /* Chomp, then ignore empty/comment lines. */
                if (((len=strlen(buf)) > 0) && (buf[len-1] == '\n'))
                    buf[--len] = '\0';

                if ((len == 0) || (buf[0] == '#'))
                    continue;

                if (do_user(buf)) {
                    if (verbose)
                        fprintf(stderr,
                                "Error from do_user(): bailing out!\n");
                    syslog(LOG_ERR, "Error in do_sync(%s): bailing out!", buf);
                    exit_rc = 1;
                    break;
                }
            }
            fclose(file);
        } else for (i = optind; i < argc; i++) {
            if (do_user(argv[i])) {
                if (verbose)
                    fprintf(stderr, "Error from do_user(): bailing out!\n");
                syslog(LOG_ERR, "Error in do_sync(%s): bailing out!", argv[i]);
                exit_rc = 1;
                break;
            }
        }
    } else if (mailbox) {
        struct sync_user_list *user_list = sync_user_list_create();
        struct sync_user   *user;
        char   *s, *t;

        if (input_filename) {
            if ((file=fopen(input_filename, "r")) == NULL) {
                syslog(LOG_NOTICE, "Unable to open %s: %m", input_filename);
                shut_down(1);
            }
            while (fgets(buf, sizeof(buf), file)) {
                /* Chomp, then ignore empty/comment lines. */
                if (((len=strlen(buf)) > 0) && (buf[len-1] == '\n'))
                    buf[--len] = '\0';

                if ((len == 0) || (buf[0] == '#'))
                    continue;

                if (strncmp(argv[i], "user.", 5) != 0)
                    continue;

                s = argv[i]+5;

                if ((t = strchr(s, '.'))) {
                    if ((t-s) >= 31) continue;

                    memcpy(buf, s, t-s);
                    buf[t-s] = '\0';
                } else
                    strcpy(buf, s);

                if (!(user = sync_user_list_lookup(user_list, buf)))
                    user = sync_user_list_add(user_list, buf);

                if (!sync_folder_lookup_byname(user->folder_list, argv[i]))
                    sync_folder_list_add(user->folder_list,
                                         NULL, argv[i], NULL);
            }
            fclose(file);
        } else for (i = optind; i < argc; i++) {
            if (strncmp(argv[i], "user.", 5) != 0)
                continue;

            s = argv[i]+5;

            if ((t = strchr(s, '.'))) {
                if ((t-s) >= 31) continue;

                memcpy(buf, s, t-s);
                buf[t-s] = '\0';
            } else
                strcpy(buf, s);

            if (!(user = sync_user_list_lookup(user_list, buf)))
                user = sync_user_list_add(user_list, buf);

            if (!sync_folder_lookup_byname(user->folder_list, argv[i]))
                sync_folder_list_add(user->folder_list, NULL, argv[i], NULL);
        }

        for (user = user_list->head ; user ; user = user->next) {
            if (do_mailboxes(user->userid, user->folder_list)) {
                if (verbose) {
                    fprintf(stderr,
                            "Error from do_mailboxes(): bailing out!\n");
                }
                syslog(LOG_ERR, "Error in do_mailboxes(%s): bailing out!",
                       user->userid);
                exit_rc = 1;
                break;
            }
        }

        sync_user_list_free(&user_list);
    } else if (sieve) {
        for (i = optind; i < argc; i++) {
            if (do_sieve(argv[i])) {
                if (verbose) {
                    fprintf(stderr,
                            "Error from do_sieve(): bailing out!\n");
                }
                syslog(LOG_ERR, "Error in do_sieve(%s): bailing out!",
                       argv[i]);
                exit_rc = 1;
                break;
            }
        }
    } else if (repeat)
        do_daemon(sync_log_file, sync_shutdown_file, timeout, min_delta);
    else if (verbose)
        fprintf(stderr, "Nothing to do!\n");

    sync_msgid_list_free(&msgid_onserver);
    backend_disconnect(be);

  quit:
    shut_down(exit_rc);
}