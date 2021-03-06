/* append.c -- Routines for appending messages to a mailbox
 *
 * Copyright (c) 1994-2008 Carnegie Mellon University.  All rights reserved.
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

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/poll.h>

#include "acl.h"
#include "assert.h"
#include "mailbox.h"
#include "message.h"
#include "append.h"
#include "global.h"
#include "prot.h"
#include "sync_log.h"
#include "xmalloc.h"
#include "xstrlcpy.h"
#include "xstrlcat.h"
#include "mboxlist.h"
#include "seen.h"
#include "retry.h"
#include "quota.h"
#include "util.h"

/* generated headers are not necessarily in current directory */
#include "imap/imap_err.h"

#include "annotate.h"
#include "message_guid.h"
#include "strarray.h"
#include "conversations.h"
#include "objectstore.h"

struct stagemsg {
    char fname[1024];

    strarray_t parts; /* buffer of current stage parts */
    struct message_guid guid;
};

static int append_addseen(struct mailbox *mailbox, const char *userid,
                          struct seqset *newseen);
static void append_setseen(struct appendstate *as, struct index_record *record);

#define zero_index(i) { memset(&i, 0, sizeof(struct index_record)); }

/*
 * Check to see if mailbox can be appended to
 *
 * Arguments:
 *      name       - name of mailbox directory
 *      aclcheck   - user must have these rights on mailbox ACL
 *      quotastorage_check - mailbox must have this much storage quota left
 *                   (-1 means don't care about quota)
 *      quotamessage_check - mailbox must have this much message quota left
 *                   (-1 means don't care about quota)
 *
 */
EXPORTED int append_check(const char *name,
                 struct auth_state *auth_state,
                 long aclcheck,
                 const quota_t quotacheck[QUOTA_NUMRESOURCES])
{
    struct mailbox *mailbox = NULL;
    int myrights;
    int r;

    r = mailbox_open_irl(name, &mailbox);
    if (r) return r;

    myrights = cyrus_acl_myrights(auth_state, mailbox->acl);

    if ((myrights & aclcheck) != aclcheck) {
        r = (myrights & ACL_LOOKUP) ?
          IMAP_PERMISSION_DENIED : IMAP_MAILBOX_NONEXISTENT;
        goto done;
    }

    if (quotacheck)
        r = mailbox_quota_check(mailbox, quotacheck);

done:
    mailbox_close(&mailbox);

    return r;
}

/*
 * Open a mailbox for appending
 *
 * Arguments:
 *      name       - name of mailbox directory
 *      aclcheck   - user must have these rights on mailbox ACL
 *      quotastorage_check - mailbox must have this much storage quota left
 *                   (-1 means don't care about quota)
 *      quotamessage_check - mailbox must have this much message quota left
 *                   (-1 means don't care about quota)
 *      event_type - the event among MessageNew, MessageAppend and
 *                   vnd.cmu.MessageCopy (zero means don't send notification)
 * On success, the struct pointed to by 'as' is set up.
 *
 * when you commit or abort, the mailbox is closed
 */
EXPORTED int append_setup(struct appendstate *as, const char *name,
                 const char *userid, const struct auth_state *auth_state,
                 long aclcheck, const quota_t quotacheck[QUOTA_NUMRESOURCES],
                 const struct namespace *namespace, int isadmin, enum event_type  event_type)
{
    int r;
    struct mailbox *mailbox = NULL;

    r = mailbox_open_iwl(name, &mailbox);
    if (r) return r;

    r = append_setup_mbox(as, mailbox, userid, auth_state,
                          aclcheck, quotacheck, namespace, isadmin, event_type);
    if (r) mailbox_close(&mailbox);
    else as->close_mailbox_when_done = 1;

    return r;
}

/* setup for append with an existing mailbox
 *
 * same as append_setup, but when you commit, the mailbox remains open and locked.
 *
 * Requires as write locked mailbox (of course)
 */
EXPORTED int append_setup_mbox(struct appendstate *as, struct mailbox *mailbox,
                               const char *userid, const struct auth_state *auth_state,
                               long aclcheck, const quota_t quotacheck[QUOTA_NUMRESOURCES],
                               const struct namespace *namespace, int isadmin,
                               enum event_type event_type)
{
    int r;

    memset(as, 0, sizeof(*as));

    as->myrights = cyrus_acl_myrights(auth_state, mailbox->acl);

    if ((as->myrights & aclcheck) != aclcheck) {
        r = (as->myrights & ACL_LOOKUP) ?
          IMAP_PERMISSION_DENIED : IMAP_MAILBOX_NONEXISTENT;
        return r;
    }

    if (quotacheck) {
        r = mailbox_quota_check(mailbox, quotacheck);
        if (r) return r;
    }

    if (userid) {
        strlcpy(as->userid, userid, sizeof(as->userid));
    } else {
        as->userid[0] = '\0';
    }
    as->namespace = namespace;
    as->auth_state = auth_state;
    as->isadmin = isadmin;

    /* initialize seen list creator */
    as->internalseen = mailbox_internal_seen(mailbox, as->userid);
    as->seen_seq = seqset_init(0, SEQ_SPARSE);

    /* zero out metadata */
    as->nummsg = 0;
    as->baseuid = mailbox->i.last_uid + 1;
    as->s = APPEND_READY;

    as->event_type = event_type;
    as->mboxevents = NULL;

    as->mailbox = mailbox;

    return 0;
}

EXPORTED uint32_t append_uidvalidity(struct appendstate *as)
{
    return as->mailbox->i.uidvalidity;
}

static void append_free(struct appendstate *as)
{
    if (!as) return;
    if (as->s == APPEND_DONE) return;

    seqset_free(as->seen_seq);
    as->seen_seq = NULL;

    mboxevent_freequeue(&as->mboxevents);
    as->event_type = 0;

    if (as->close_mailbox_when_done)
        mailbox_close(&as->mailbox);

    as->s = APPEND_DONE;
}

/* may return non-zero, indicating that the entire append has failed
 and the mailbox is probably in an inconsistent state. */
EXPORTED int append_commit(struct appendstate *as)
{
    int r = 0;

    if (as->s == APPEND_DONE) return 0;

    if (as->nummsg) {
        /* Calculate new index header information */
        as->mailbox->i.last_appenddate = time(0);

        /* log the append so rolling squatter can index this mailbox */
        sync_log_append(as->mailbox->name);

        /* set seen state */
        if (as->userid[0])
            append_addseen(as->mailbox, as->userid, as->seen_seq);
    }

    /* We want to commit here to guarantee mailbox on disk vs
     * duplicate DB consistency */
    r = mailbox_commit(as->mailbox);
    if (r) {
        syslog(LOG_ERR, "IOERROR: commiting mailbox append %s: %s",
               as->mailbox->name, error_message(r));
        append_abort(as);
        return r;
    }

    /* send the list of MessageCopy or MessageAppend event notifications at once */
    mboxevent_notify(as->mboxevents);

    append_free(as);
    return 0;
}

/* may return non-zero, indicating an internal error of some sort. */
EXPORTED int append_abort(struct appendstate *as)
{
    if (as->s == APPEND_DONE) return 0;
    append_free(as);

    return 0;
}

/*
 * staging, to allow for single-instance store.  initializes the stage
 * with the file for the given mailboxname and returns the open file
 * so it can double as the spool file
 */
EXPORTED FILE *append_newstage(const char *mailboxname, time_t internaldate,
                      int msgnum, struct stagemsg **stagep)
{
    struct stagemsg *stage;
    char stagedir[MAX_MAILBOX_PATH+1], stagefile[MAX_MAILBOX_PATH+1];
    FILE *f;
    int r;

    assert(mailboxname != NULL);
    assert(stagep != NULL);

    *stagep = NULL;

    stage = xmalloc(sizeof(struct stagemsg));
    strarray_init(&stage->parts);

    snprintf(stage->fname, sizeof(stage->fname), "%d-%d-%d",
             (int) getpid(), (int) internaldate, msgnum);

    r = mboxlist_findstage(mailboxname, stagedir, sizeof(stagedir));
    if (r) {
        syslog(LOG_ERR, "couldn't find stage directory for mbox: '%s': %s",
               mailboxname, error_message(r));
        free(stage);
        return NULL;
    }
    strlcpy(stagefile, stagedir, sizeof(stagefile));
    strlcat(stagefile, stage->fname, sizeof(stagefile));

    /* create this file and put it into stage->parts[0] */
    unlink(stagefile);
    f = fopen(stagefile, "w+");
    if (!f) {
        if (mkdir(stagedir, 0755) != 0) {
            syslog(LOG_ERR, "couldn't create stage directory: %s: %m",
                   stagedir);
        } else {
            syslog(LOG_NOTICE, "created stage directory %s",
                   stagedir);
            f = fopen(stagefile, "w+");
        }
    }
    if (!f) {
        syslog(LOG_ERR, "IOERROR: creating message file %s: %m",
               stagefile);
        strarray_fini(&stage->parts);
        free(stage);
        return NULL;
    }

    strarray_append(&stage->parts, stagefile);

    *stagep = stage;
    return f;
}

/*
 * Send the args down a socket.  We use a counted encoding
 * similar in concept to HTTP chunked encoding, with a decimal
 * ASCII encoded length followed by that many bytes of data.
 * A zero length indicates end of message.
 */
static int callout_send_args(int fd, const struct buf *args)
{
    char lenbuf[32];
    int r = 0;

    snprintf(lenbuf, sizeof(lenbuf), "%u\n", (unsigned)args->len);
    r = retry_write(fd, lenbuf, strlen(lenbuf));
    if (r < 0)
        goto out;

    if (args->len) {
        r = retry_write(fd, args->s, args->len);
        if (r < 0)
            goto out;
        r = retry_write(fd, "0\n", 2);
    }

out:
    return (r < 0 ? IMAP_SYS_ERROR : 0);
}

#define CALLOUT_TIMEOUT_MS      (10*1000)

static int callout_receive_reply(const char *callout,
                                 int fd, struct dlist **results)
{
    struct protstream *p = NULL;
    int r;
    int c;
    struct pollfd pfd;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN;

    r = poll(&pfd, 1, CALLOUT_TIMEOUT_MS);
    if (r < 0) {
        syslog(LOG_ERR, "cannot poll() waiting for callout %s: %m",
               callout);
        r = IMAP_SYS_ERROR;
        goto out;
    }
    if (r == 0) {
        syslog(LOG_ERR, "timed out waiting for callout %s",
               callout);
        r = IMAP_SYS_ERROR;
        goto out;
    }

    p = prot_new(fd, /*write*/0);
    prot_setisclient(p, 1);

    /* read and parse the reply as a dlist */
    c = dlist_parse(results, /*flags*/0, p);
    r = (c == EOF ? IMAP_SYS_ERROR : 0);

out:
    if (p)
        prot_free(p);
    return r;
}

/*
 * Handle the callout as a service listening on a UNIX domain socket.
 * Send the encoded arguments down the socket; capture the reply and
 * decode it as a dlist.
 */
static int callout_run_socket(const char *callout,
                              const struct buf *args,
                              struct dlist **results)
{
    int sock = -1;
    struct sockaddr_un mysun;
    int r;

    sock = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        syslog(LOG_ERR, "cannot create socket for callout: %m");
        r = IMAP_SYS_ERROR;
        goto out;
    }

    memset(&mysun, 0, sizeof(mysun));
    mysun.sun_family = AF_UNIX;
    strncpy(mysun.sun_path, callout, sizeof(mysun.sun_path));
    r = connect(sock, (struct sockaddr *)&mysun, sizeof(mysun));
    if (r < 0) {
        syslog(LOG_ERR, "cannot connect socket for callout: %m");
        r = IMAP_SYS_ERROR;
        goto out;
    }

    r = callout_send_args(sock, args);
    if (r)
        goto out;

    r = callout_receive_reply(callout, sock, results);

out:
    if (sock >= 0)
        close(sock);
    return r;
}

/*
 * Handle the callout as an executable.  Fork and exec the callout as an
 * executable, with the encoded arguments appearing on stdin and the
 * stdout captured as a dlist.
 */
static int callout_run_executable(const char *callout,
                                  const struct buf *args,
                                  struct dlist **results)
{
    pid_t pid, reaped;
#define PIPE_READ    0
#define PIPE_WRITE   1
    int inpipe[2] = { -1, -1 };
    int outpipe[2] = { -1, -1 };
    int status;
    int r;

    r = pipe(inpipe);
    if (!r)
        r = pipe(outpipe);
    if (r < 0) {
        syslog(LOG_ERR, "cannot create pipe for callout: %m");
        r = IMAP_SYS_ERROR;
        goto out;
    }

    pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "cannot fork for callout: %m");
        r = IMAP_SYS_ERROR;
        goto out;
    }

    if (pid == 0) {
        /* child process */

        close(inpipe[PIPE_WRITE]);
        dup2(inpipe[PIPE_READ], /*FILENO_STDIN*/0);
        close(inpipe[PIPE_READ]);

        close(outpipe[PIPE_READ]);
        dup2(outpipe[PIPE_WRITE], /*FILENO_STDOUT*/1);
        close(outpipe[PIPE_WRITE]);

        execl(callout, callout, (char *)NULL);
        syslog(LOG_ERR, "cannot exec callout %s: %m", callout);
        exit(1);
    }
    /* parent process */
    close(inpipe[PIPE_READ]);
    inpipe[PIPE_READ] = -1;
    close(outpipe[PIPE_WRITE]);
    outpipe[PIPE_WRITE] = -1;

    r = callout_send_args(inpipe[PIPE_WRITE], args);
    if (r)
        goto out;

    r = callout_receive_reply(callout, outpipe[PIPE_READ], results);
    if (r)
        goto out;

    /* reap the child process */
    do {
        reaped = waitpid(pid, &status, 0);
        if (reaped < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ESRCH)
                break;
            if (errno == ECHILD)
                break;
            syslog(LOG_ERR, "error reaping callout pid %d: %m",
                    (int)pid);
            r = IMAP_SYS_ERROR;
            goto out;
        }
    }
    while (reaped != pid);
    r = 0;

out:
    if (inpipe[PIPE_READ] >= 0)
        close(inpipe[PIPE_READ]);
    if (inpipe[PIPE_WRITE] >= 0)
        close(inpipe[PIPE_WRITE]);
    if (outpipe[PIPE_READ] >= 0)
        close(outpipe[PIPE_READ]);
    if (outpipe[PIPE_WRITE] >= 0)
        close(outpipe[PIPE_WRITE]);
    return r;
#undef PIPE_READ
#undef PIPE_WRITE
}

/*
 * Encode the arguments for a callout into @buf.
 */
static void callout_encode_args(struct buf *args,
                                const char *fname,
                                const struct body *body,
                                struct entryattlist *annotations,
                                strarray_t *flags)
{
    struct entryattlist *ee;
    int i;

    buf_putc(args, '(');

    buf_printf(args, "FILENAME ");
    message_write_nstring(args, fname);

    buf_printf(args, " ANNOTATIONS (");
    for (ee = annotations ; ee ; ee = ee->next) {
        struct attvaluelist *av;
        message_write_nstring(args, ee->entry);
        buf_putc(args, ' ');
        buf_putc(args, '(');
        for (av = ee->attvalues ; av ; av = av->next) {
            message_write_nstring(args, av->attrib);
            buf_putc(args, ' ');
            message_write_nstring_map(args, av->value.s, av->value.len);
            if (av->next)
                buf_putc(args, ' ');
        }
        buf_putc(args, ')');
        if (ee->next)
            buf_putc(args, ' ');
    }
    buf_putc(args, ')');

    buf_printf(args, " FLAGS (");
    for (i = 0 ; i < flags->count ; i++) {
        if (i)
            buf_putc(args, ' ');
        buf_appendcstr(args, flags->data[i]);
    }
    buf_putc(args, ')');

    buf_appendcstr(args, " BODY ");
    message_write_body(args, body, 2);

    buf_printf(args, " GUID %s", message_guid_encode(&body->guid));
    buf_putc(args, ')');
    buf_cstring(args);
}

/*
 * Parse the reply from the callout.  This designed to be similar to the
 * arguments of the STORE command, except that we can have multiple
 * items one after the other and the whole thing is in a list.
 *
 * Examples:
 * (+FLAGS \Flagged)
 * (+FLAGS (\Flagged \Seen))
 * (-FLAGS \Flagged)
 * (ANNOTATION (/comment (value.shared "Hello World")))
 * (+FLAGS \Flagged ANNOTATION (/comment (value.shared "Hello")))
 *
 * The result is merged into @user_annots, @system_annots, and @flags.
 * User-set annotations are kept separate from system-set annotations
 * for two reasons: a) system-set annotations need to bypass the ACL
 * check to allow them to work during local delivery, and b) failure
 * to set system-set annotations needs to be logged but must not cause
 * the append to fail.
 */
static void callout_decode_results(const char *callout,
                                   const struct dlist *results,
                                   struct entryattlist **user_annots,
                                   struct entryattlist **system_annots,
                                   strarray_t *flags)
{
    struct dlist *dd;

    for (dd = results->head ; dd ; dd = dd->next) {
        const char *key = dlist_cstring(dd);
        const char *val;
        dd = dd->next;
        if (!dd)
            goto error;

        if (!strcasecmp(key, "+FLAGS")) {
            if (dd->head) {
                struct dlist *dflag;
                for (dflag = dd->head ; dflag ; dflag = dflag->next)
                    if ((val = dlist_cstring(dflag)))
                        strarray_add_case(flags, val);
            }
            else if ((val = dlist_cstring(dd))) {
                strarray_add_case(flags, val);
            }
        }
        else if (!strcasecmp(key, "-FLAGS")) {
            if (dd->head) {
                struct dlist *dflag;
                for (dflag = dd->head ; dflag ; dflag = dflag->next) {
                    if ((val = dlist_cstring(dflag)))
                        strarray_remove_all_case(flags, val);
                }
            }
            else if ((val = dlist_cstring(dd))) {
                strarray_remove_all_case(flags, val);
            }
        }
        else if (!strcasecmp(key, "ANNOTATION")) {
            const char *entry;
            struct dlist *dx = dd->head;

            if (!dx)
                goto error;
            entry = dlist_cstring(dx);
            if (!entry)
                goto error;

            for (dx = dx->next ; dx ; dx = dx->next) {
                const char *attrib;
                const char *valmap;
                size_t vallen;
                struct buf value = BUF_INITIALIZER;

                /* must be a list with exactly two elements,
                 * an attrib and a value */
                if (!dx->head || !dx->head->next || dx->head->next->next)
                    goto error;
                attrib = dlist_cstring(dx->head);
                if (!attrib)
                    goto error;
                if (!dlist_tomap(dx->head->next, &valmap, &vallen))
                    goto error;
                buf_init_ro(&value, valmap, vallen);
                clearentryatt(user_annots, entry, attrib);
                setentryatt(system_annots, entry, attrib, &value);
                buf_free(&value);
            }
        }
        else {
            goto error;
        }
    }

    return;
error:
    syslog(LOG_WARNING, "Unexpected data in response from callout %s",
           callout);
}

static int callout_run(const char *fname,
                       const struct body *body,
                       struct entryattlist **user_annots,
                       struct entryattlist **system_annots,
                       strarray_t *flags)
{
    const char *callout;
    struct stat sb;
    struct buf args = BUF_INITIALIZER;
    struct dlist *results = NULL;
    int r;

    callout = config_getstring(IMAPOPT_ANNOTATION_CALLOUT);
    assert(callout);
    assert(flags);

    callout_encode_args(&args, fname, body, *user_annots, flags);

    if (stat(callout, &sb) < 0) {
        syslog(LOG_ERR, "cannot stat annotation_callout %s: %m", callout);
        r = IMAP_IOERROR;
        goto out;
    }
    if (S_ISSOCK(sb.st_mode)) {
        /* UNIX domain socket on which a service is listening */
        r = callout_run_socket(callout, &args, &results);
        if (r)
            goto out;
    }
    else if (S_ISREG(sb.st_mode) &&
             (sb.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))) {
        /* regular file, executable */
        r = callout_run_executable(callout, &args, &results);
        if (r)
            goto out;
    }
    else {
        syslog(LOG_ERR, "cannot classify annotation_callout %s", callout);
        r = IMAP_IOERROR;
        goto out;
    }

    if (results) {
        /* We have some results, parse them and merge them back into
         * the annotations and flags we were given */
        callout_decode_results(callout, results,
                               user_annots, system_annots, flags);
    }

out:
    buf_free(&args);
    dlist_free(&results);

    return r;
}

static int append_apply_flags(struct appendstate *as,
                              struct mboxevent *mboxevent,
                              struct index_record *record,
                              const strarray_t *flags)
{
    int userflag;
    int i, r = 0;

    assert(flags);

    for (i = 0; i < flags->count; i++) {
        const char *flag = strarray_nth(flags, i);
        if (!strcasecmp(flag, "\\seen")) {
            append_setseen(as, record);
            mboxevent_add_flag(mboxevent, flag);
        }
        else if (!strcasecmp(flag, "\\deleted")) {
            if (as->myrights & ACL_DELETEMSG) {
                record->system_flags |= FLAG_DELETED;
                mboxevent_add_flag(mboxevent, flag);
            }
        }
        else if (!strcasecmp(flag, "\\draft")) {
            if (as->myrights & ACL_WRITE) {
                record->system_flags |= FLAG_DRAFT;
                mboxevent_add_flag(mboxevent, flag);
            }
        }
        else if (!strcasecmp(flag, "\\flagged")) {
            if (as->myrights & ACL_WRITE) {
                record->system_flags |= FLAG_FLAGGED;
                mboxevent_add_flag(mboxevent, flag);
            }
        }
        else if (!strcasecmp(flag, "\\answered")) {
            if (as->myrights & ACL_WRITE) {
                record->system_flags |= FLAG_ANSWERED;
                mboxevent_add_flag(mboxevent, flag);
            }
        }
        else if (as->myrights & ACL_WRITE) {
            r = mailbox_user_flag(as->mailbox, flag, &userflag, 1);
            if (r) goto out;
            record->user_flags[userflag/32] |= 1<<(userflag&31);
            mboxevent_add_flag(mboxevent, flag);
        }
    }

out:
    return r;
}

/*
 * staging, to allow for single-instance store.  the complication here
 * is multiple partitions.
 *
 * Note: @user_annots needs to be freed by the caller but
 * may be modified during processing of callout responses.
 */
EXPORTED int append_fromstage(struct appendstate *as, struct body **body,
                     struct stagemsg *stage, time_t internaldate,
                     const strarray_t *flags, int nolink,
                     struct entryattlist *user_annots)
{
    struct mailbox *mailbox = as->mailbox;
    struct index_record record;
    const char *fname;
    int i, r;
    strarray_t *newflags = NULL;
    struct entryattlist *system_annots = NULL;
    struct mboxevent *mboxevent = NULL;
    int object_storage_enabled = config_getswitch(IMAPOPT_OBJECT_STORAGE_ENABLED) ;

    /* for staging */
    char stagefile[MAX_MAILBOX_PATH+1];

    assert(stage != NULL && stage->parts.count);

    /* parse the first file */
    if (!*body) {
        FILE *file = fopen(stage->parts.data[0], "r");
        if (file) {
            r = message_parse_file(file, NULL, NULL, body);
            fclose(file);
        }
        else
            r = IMAP_IOERROR;
        if (r) goto out;
    }

    zero_index(record);

    /* xxx check errors */
    mboxlist_findstage(mailbox->name, stagefile, sizeof(stagefile));
    strlcat(stagefile, stage->fname, sizeof(stagefile));

    for (i = 0 ; i < stage->parts.count ; i++) {
        /* ok, we've successfully created the file */
        if (!strcmp(stagefile, stage->parts.data[i])) {
            /* aha, this is us */
            break;
        }
    }

    if (i == stage->parts.count) {
        /* ok, create this file, and copy the name of it into stage->parts. */

        /* create the new staging file from the first stage part */
        r = mailbox_copyfile(stage->parts.data[0], stagefile, 0);
        if (r) {
            /* maybe the directory doesn't exist? */
            char stagedir[MAX_MAILBOX_PATH+1];

            /* xxx check errors */
            mboxlist_findstage(mailbox->name, stagedir, sizeof(stagedir));
            if (mkdir(stagedir, 0755) != 0) {
                syslog(LOG_ERR, "couldn't create stage directory: %s: %m",
                       stagedir);
            } else {
                syslog(LOG_NOTICE, "created stage directory %s",
                       stagedir);
                r = mailbox_copyfile(stage->parts.data[0], stagefile, 0);
            }
        }
        if (r) {
            /* oh well, we tried */

            syslog(LOG_ERR, "IOERROR: creating message file %s: %m",
                   stagefile);
            unlink(stagefile);
            goto out;
        }

        strarray_append(&stage->parts, stagefile);
    }

    /* 'stagefile' contains the message and is on the same partition
       as the mailbox we're looking at */

    /* Setup */
    record.uid = as->baseuid + as->nummsg;
    record.internaldate = internaldate;

    /* prepare a new notification for this appended message
     * the event type must be set with MessageNew or MessageAppend */
    if (as->event_type) {
        mboxevent = mboxevent_enqueue(as->event_type, &as->mboxevents);
    }

    /* we need to parse the record first */
    r = message_create_record(&record, *body);
    if (r) goto out;

    /* should we archive it straight away? */
    if (mailbox_should_archive(mailbox, &record, NULL))
        record.system_flags |= FLAG_ARCHIVED;

    /* Create message file */
    as->nummsg++;
    fname = mailbox_record_fname(mailbox, &record);

    r = mailbox_copyfile(stagefile, fname, nolink);
    if (r) goto out;

    FILE *destfile = fopen(fname, "r");
    if (destfile) {
        /* this will hopefully ensure that the link() actually happened
           and makes sure that the file actually hits disk */
        fsync(fileno(destfile));
        fclose(destfile);
    }
    else {
        r = IMAP_IOERROR;
        goto out;
    }

    if (config_getstring(IMAPOPT_ANNOTATION_CALLOUT)) {
        if (flags)
            newflags = strarray_dup(flags);
        else
            newflags = strarray_new();
        r = callout_run(fname, *body, &user_annots, &system_annots, newflags);
        if (r) {
            syslog(LOG_ERR, "Annotation callout failed, ignoring\n");
            r = 0;
        }
        flags = newflags;
    }

    /* straight to archive? */
    int in_object_storage = 0;
    if (object_storage_enabled && record.system_flags & FLAG_ARCHIVED) {
        if (!record.internaldate)
            record.internaldate = time(NULL);
        r = objectstore_put(mailbox, &record, fname);
        if (r) {
            // didn't manage to store it, so remove the ARCHIVED flag
            record.system_flags &= ~FLAG_ARCHIVED;
            r = 0;
        }
        else {
            in_object_storage = 1;
        }
    }

    /* Handle flags the user wants to set in the message */
    if (flags) {
        r = append_apply_flags(as, mboxevent, &record, flags);
        if (r) {
            syslog(LOG_ERR, "Annotation callout failed to apply flags %s", error_message(r));
            goto out;
        }
    }

    /* Write out index file entry */
    r = mailbox_append_index_record(mailbox, &record);
    if (r) goto out;

    if (in_object_storage) {  // must delete local file
        if (unlink(fname) != 0) // unlink should do it.
            if (!remove (fname))  // we must insist
                syslog(LOG_ERR, "Removing local file <%s> error \n", fname);
    }

    /* Apply the annotations afterwards, so that the record already exists */
    if (user_annots || system_annots) {
        annotate_state_t *astate = NULL;
        r = mailbox_get_annotate_state(as->mailbox, record.uid, &astate);
        if (r) goto out;
        if (user_annots) {
            annotate_state_set_auth(astate, as->isadmin,
                                    as->userid, as->auth_state);
            r = annotate_state_store(astate, user_annots);
        }
        if (r) {
            syslog(LOG_ERR, "Annotation callout failed to apply user annots %s", error_message(r));
            goto out;
        }
        if (system_annots) {
            /* pretend to be admin to avoid ACL checks */
            annotate_state_set_auth(astate, /*isadmin*/1,
                                    as->userid, as->auth_state);
            r = annotate_state_store(astate, system_annots);
        }
        if (r) {
            syslog(LOG_ERR, "Annotation callout failed to apply system annots %s", error_message(r));
            goto out;
        }
    }

out:
    if (newflags)
        strarray_free(newflags);
    freeentryatts(system_annots);
    if (r) {
        append_abort(as);
        return r;
    }

    /* finish filling the event notification */
    /* XXX avoid to parse ENVELOPE record since Message-Id is already
     * present in body structure ? */
    mboxevent_extract_record(mboxevent, mailbox, &record);
    mboxevent_extract_mailbox(mboxevent, mailbox);
    mboxevent_set_access(mboxevent, NULL, NULL, as->userid, as->mailbox->name, 1);
    mboxevent_set_numunseen(mboxevent, mailbox, -1);

    return 0;
}

EXPORTED int append_removestage(struct stagemsg *stage)
{
    char *p;

    if (stage == NULL) return 0;

    while ((p = strarray_pop(&stage->parts))) {
        /* unlink the staging file */
        if (unlink(p) != 0) {
            syslog(LOG_ERR, "IOERROR: error unlinking file %s: %m", p);
        }
        free(p);
    }

    strarray_fini(&stage->parts);
    free(stage);
    return 0;
}

/*
 * Append to 'mailbox' from the prot stream 'messagefile'.
 * 'mailbox' must have been opened with append_setup().
 * 'size' is the expected size of the message.
 * 'internaldate' specifies the internaldate for the new message.
 * 'flags' contains the names of the 'nflags' flags that the
 * user wants to set in the message.  If the '\Seen' flag is
 * in 'flags', then the 'userid' passed to append_setup controls whose
 * \Seen flag gets set.
 *
 * The message is not committed to the mailbox (nor is the mailbox
 * unlocked) until append_commit() is called.  multiple
 * append_onefromstream()s can be aborted by calling append_abort().
 */
EXPORTED int append_fromstream(struct appendstate *as, struct body **body,
                      struct protstream *messagefile,
                      unsigned long size,
                      time_t internaldate,
                      const strarray_t *flags)
{
    struct mailbox *mailbox = as->mailbox;
    struct index_record record;
    const char *fname;
    FILE *destfile;
    int r;
    struct mboxevent *mboxevent = NULL;

    assert(size != 0);

    zero_index(record);
    /* Setup */
    record.uid = as->baseuid + as->nummsg;
    record.internaldate = internaldate;

    /* Create message file */
    fname = mailbox_record_fname(mailbox, &record);
    as->nummsg++;

    unlink(fname);
    destfile = fopen(fname, "w+");
    if (!destfile) {
        syslog(LOG_ERR, "IOERROR: creating message file %s: %m", fname);
        r = IMAP_IOERROR;
        goto out;
    }

    /* prepare a new notification for this appended message
     * the event type must be set with MessageNew or MessageAppend */
    if (as->event_type) {
        mboxevent = mboxevent_enqueue(as->event_type, &as->mboxevents);
    }

    /* XXX - also stream to stage directory and check out archive options */

    /* Copy and parse message */
    r = message_copy_strict(messagefile, destfile, size, 0);
    if (!r) {
        if (!*body || (as->nummsg - 1))
            r = message_parse_file(destfile, NULL, NULL, body);
        if (!r) r = message_create_record(&record, *body);

        /* messageContent may be included with MessageAppend and MessageNew */
        if (!r)
            mboxevent_extract_content(mboxevent, &record, destfile);
    }
    fclose(destfile);
    if (r) goto out;

    /* Handle flags the user wants to set in the message */
    if (flags) {
        r = append_apply_flags(as, mboxevent, &record, flags);
        if (r) goto out;
    }

    /* Write out index file entry; if we abort later, it's not
       important */
    r = mailbox_append_index_record(mailbox, &record);

out:
    if (r) {
        append_abort(as);
        return r;
    }

    /* finish filling the event notification */
    /* XXX avoid to parse ENVELOPE record since Message-Id is already
     * present in body structure */
    mboxevent_extract_record(mboxevent, mailbox, &record);
    mboxevent_extract_mailbox(mboxevent, mailbox);
    mboxevent_set_access(mboxevent, NULL, NULL, as->userid, as->mailbox->name, 1);
    mboxevent_set_numunseen(mboxevent, mailbox, -1);

    return 0;
}

HIDDEN int append_run_annotator(struct appendstate *as,
                         struct index_record *record)
{
    FILE *f = NULL;
    const char *fname;
    struct entryattlist *user_annots = NULL;
    struct entryattlist *system_annots = NULL;
    strarray_t *flags = NULL;
    annotate_state_t *astate = NULL;
    struct body *body = NULL;
    int r = 0;

    if (!config_getstring(IMAPOPT_ANNOTATION_CALLOUT))
        return 0;

    flags = mailbox_extract_flags(as->mailbox, record, as->userid);
    user_annots = mailbox_extract_annots(as->mailbox, record);

    fname = mailbox_record_fname(as->mailbox, record);
    if (!fname) goto out;

    f = fopen(fname, "r");
    if (!f) {
        r = IMAP_IOERROR;
        goto out;
    }

    r = message_parse_file(f, NULL, NULL, &body);
    if (r) goto out;

    fclose(f);
    f = NULL;

    r = callout_run(fname, body, &user_annots, &system_annots, flags);
    if (r) goto out;

    record->system_flags &= (FLAG_SEEN | FLAGS_INTERNAL);
    memset(&record->user_flags, 0, sizeof(record->user_flags));
    r = append_apply_flags(as, NULL, record, flags);
    if (r) {
        syslog(LOG_ERR, "Setting flags from annotator "
                        "callout failed (%s)",
                        error_message(r));
        goto out;
    }

    r = mailbox_get_annotate_state(as->mailbox, record->uid, &astate);
    if (r) goto out;
    if (user_annots) {
        annotate_state_set_auth(astate, as->isadmin,
                                as->userid, as->auth_state);
        r = annotate_state_store(astate, user_annots);
        if (r) {
            syslog(LOG_ERR, "Setting user annnotations from annotator "
                            "callout failed (%s)",
                            error_message(r));
            goto out;
        }
    }
    if (system_annots) {
        /* pretend to be admin to avoid ACL checks */
        annotate_state_set_auth(astate, /*isadmin*/1,
                                as->userid, as->auth_state);
        r = annotate_state_store(astate, system_annots);
        if (r) {
            syslog(LOG_ERR, "Setting system annnotations from annotator "
                            "callout failed (%s)",
                            error_message(r));
            goto out;
        }
    }

out:
    if (f) fclose(f);
    freeentryatts(user_annots);
    freeentryatts(system_annots);
    strarray_free(flags);
    if (body) {
        message_free_body(body);
        free(body);
    }
    return r;
}

/*
 * Append to 'as->mailbox' the 'nummsg' messages from the
 * mailbox 'mailbox' listed in the array pointed to by 'records'.
 * 'as' must have been opened with append_setup().  If the '\Seen'
 * flag is to be set anywhere then 'userid' passed to append_setup()
 * contains the name of the user whose \Seen flag gets set.
 */
EXPORTED int append_copy(struct mailbox *mailbox, struct appendstate *as,
                         int nummsg, struct index_record *records,
                         int nolink, int is_same_user)
{
    int msg;
    struct index_record record;
    char *srcfname = NULL;
    char *destfname = NULL;
    int object_storage_enabled = config_getswitch(IMAPOPT_OBJECT_STORAGE_ENABLED) ;
    int r = 0;
    int userflag;
    int i;
    annotate_state_t *astate = NULL;
    struct mboxevent *mboxevent = NULL;

    if (!nummsg) {
        append_abort(as);
        return 0;
    }

    /* prepare a single vnd.cmu.MessageCopy notification for all messages */
    if (as->event_type) {
        mboxevent = mboxevent_enqueue(as->event_type, &as->mboxevents);
    }

    /* Copy/link all files and cache info */
    for (msg = 0; msg < nummsg; msg++) {
        /* read in existing cache record BEFORE we copy data, so that the
         * mmap will be up to date even if it's the same mailbox for source
         * and destination */
        r = mailbox_cacherecord(mailbox, &records[msg]);
        if (r) goto out;

        record = records[msg]; /* copy data */

        /* wipe out the bits that aren't magically copied */
        record.system_flags &= ~FLAG_SEEN;
        for (i = 0; i < MAX_USER_FLAGS/32; i++)
            record.user_flags[i] = 0;
        if (!is_same_user)
            record.cid = NULLCONVERSATION;
        record.cache_offset = 0;

        /* renumber the message into the new mailbox */
        record.uid = as->mailbox->i.last_uid + 1;
        as->nummsg++;

        /* user flags are special - different numbers, so look them up */
        if (as->myrights & ACL_WRITE) {
            for (userflag = 0; userflag < MAX_USER_FLAGS; userflag++) {
                bit32 flagmask = records[msg].user_flags[userflag/32];
                if (mailbox->flagname[userflag] && (flagmask & (1<<(userflag&31)))) {
                    int num;
                    r = mailbox_user_flag(as->mailbox, mailbox->flagname[userflag], &num, 1);
                    if (r)
                        syslog(LOG_ERR, "IOERROR: unable to copy flag %s from %s to %s for UID %u: %s",
                               mailbox->flagname[userflag], mailbox->name, as->mailbox->name,
                               records[msg].uid, error_message(r));
                    else
                        record.user_flags[num/32] |= 1<<(num&31);
                }
            }
        }
        else {
            /* only flag allow to be kept without ACL_WRITE is DELETED */
            record.system_flags &= FLAG_DELETED;
        }

        /* deleted flag has its own ACL */
        if (!(as->myrights & ACL_DELETEMSG)) {
            record.system_flags &= ~FLAG_DELETED;
        }

        /* should this message be marked \Seen? */
        if (records[msg].system_flags & FLAG_SEEN) {
            append_setseen(as, &record);
        }

        /* we're not modifying the ARCHIVED flag here, just keeping it */

        /* Link/copy message file */
        free(srcfname);
        free(destfname);
        srcfname = xstrdup(mailbox_record_fname(mailbox, &records[msg]));
        destfname = xstrdup(mailbox_record_fname(as->mailbox, &record));

        r = mailbox_copyfile(srcfname, destfname, nolink);
        if (r) goto out;

        int in_object_storage = 0;
        if (object_storage_enabled && record.system_flags & FLAG_ARCHIVED) {
            r = objectstore_put(as->mailbox, &record, destfname);   // put should just add the refcount.
            if (r) {
                record.system_flags &= ~FLAG_ARCHIVED;
                r = 0;
            }
            else {
                in_object_storage = 1;
            }
        }

        /* Write out index file entry */
        r = mailbox_append_index_record(as->mailbox, &record);
        if (r) goto out;

        /* ensure we have an astate connected to the destination
         * mailbox, so that the annotation txn will be committed
         * when we close the mailbox */
        r = mailbox_get_annotate_state(as->mailbox, record.uid, &astate);
        if (r) goto out;

        r = annotate_msg_copy(mailbox, records[msg].uid,
                              as->mailbox, record.uid,
                              as->userid);
        if (r) goto out;

        if (in_object_storage) {  // must delete local file
            if (unlink(destfname) != 0) // unlink should do it.
                if (!remove (destfname))  // we must insist
                    syslog(LOG_ERR, "Removing local file <%s> error \n", destfname);
        }

        mboxevent_extract_record(mboxevent, as->mailbox, &record);
        mboxevent_extract_copied_record(mboxevent, mailbox, &records[msg]);
    }

out:
    free(srcfname);
    free(destfname);
    if (r) {
        append_abort(as);
        return r;
    }

    mboxevent_extract_mailbox(mboxevent, as->mailbox);
    mboxevent_set_access(mboxevent, NULL, NULL, as->userid, as->mailbox->name, 1);
    mboxevent_set_numunseen(mboxevent, as->mailbox, -1);

    return 0;
}

static void append_setseen(struct appendstate *as, struct index_record *record)
{
    if (as->internalseen)
        record->system_flags |= FLAG_SEEN;
    else
        seqset_add(as->seen_seq, record->uid, 1);
}

/*
 * Set the \Seen flag for 'userid' in 'mailbox' for the messages from
 * 'msgrange'.  the lowest msgrange must be larger than any previously
 * seen message.
 */
static int append_addseen(struct mailbox *mailbox,
                          const char *userid,
                          struct seqset *newseen)
{
    int r;
    struct seen *seendb = NULL;
    struct seendata sd = SEENDATA_INITIALIZER;
    struct seqset *oldseen;

    if (!newseen->len)
        return 0;

    r = seen_open(userid, SEEN_CREATE, &seendb);
    if (r) goto done;

    r = seen_lockread(seendb, mailbox->uniqueid, &sd);
    if (r) goto done;

    /* parse the old sequence */
    oldseen = seqset_parse(sd.seenuids, NULL, mailbox->i.last_uid);
    seen_freedata(&sd);

    /* add the extra items */
    seqset_join(oldseen, newseen);
    sd.seenuids = seqset_cstring(oldseen);
    seqset_free(oldseen);

    /* and write it out */
    sd.lastchange = time(NULL);
    r = seen_write(seendb, mailbox->uniqueid, &sd);
    seen_freedata(&sd);

 done:
    seen_close(&seendb);
    return r;
}

EXPORTED const char *append_stagefname(struct stagemsg *stage)
{
    return strarray_nth(&stage->parts, 0);
}
