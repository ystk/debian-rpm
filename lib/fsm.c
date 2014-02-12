/** \ingroup payload
 * \file lib/fsm.c
 * File state machine to handle a payload from a package.
 */

#include "system.h"

#include <utime.h>
#include <errno.h>
#if defined(HAVE_MMAP)
#include <sys/mman.h>
#endif
#if WITH_CAP
#include <sys/capability.h>
#endif

#include <rpm/rpmte.h>
#include <rpm/rpmts.h>
#include <rpm/rpmsq.h>
#include <rpm/rpmlog.h>

#include "rpmio/rpmio_internal.h"	/* fdInit/FiniDigest */
#include "lib/cpio.h"
#include "lib/fsm.h"
#define	fsmUNSAFE	fsmStage
#include "lib/rpmfi_internal.h"	/* XXX fi->apath, ... */
#include "lib/rpmte_internal.h"	/* XXX rpmfs */
#include "lib/rpmts_internal.h"	/* rpmtsSELabelFoo() only */
#include "lib/rpmug.h"

#include "debug.h"

#define	_FSM_DEBUG	0
int _fsm_debug = _FSM_DEBUG;

/* XXX Failure to remove is not (yet) cause for failure. */
static int strict_erasures = 0;

/** \ingroup payload
 * Keeps track of the set of all hard links to a file in an archive.
 */
struct hardLink_s {
    hardLink_t next;
    const char ** nsuffix;
    int * filex;
    struct stat sb;
    nlink_t nlink;
    nlink_t linksLeft;
    int linkIndex;
    int createdPath;
};

/** \ingroup payload
 * Iterator across package file info, forward on install, backward on erase.
 */
struct fsmIterator_s {
    rpmts ts;			/*!< transaction set. */
    rpmte te;			/*!< transaction element. */
    rpmfi fi;			/*!< transaction element file info. */
    int reverse;		/*!< reversed traversal? */
    int isave;			/*!< last returned iterator index. */
    int i;			/*!< iterator index. */
};

/**
 * Retrieve transaction set from file state machine iterator.
 * @param fsm		file state machine
 * @return		transaction set
 */
static rpmts fsmGetTs(const FSM_t fsm)
{
    const FSMI_t iter = fsm->iter;
    return (iter ? iter->ts : NULL);
}

/**
 * Retrieve transaction element file info from file state machine iterator.
 * @param fsm		file state machine
 * @return		transaction element file info
 */
static rpmfi fsmGetFi(const FSM_t fsm)
{
    const FSMI_t iter = fsm->iter;
    return (iter ? iter->fi : NULL);
}

static rpmte fsmGetTe(const FSM_t fsm)
{
    const FSMI_t iter = fsm->iter;
    return (iter ? iter->te : NULL);
}

#define	SUFFIX_RPMORIG	".rpmorig"
#define	SUFFIX_RPMSAVE	".rpmsave"
#define	SUFFIX_RPMNEW	".rpmnew"

/* Default directory and file permissions if not mapped */
#define _dirPerms 0755
#define _filePerms 0644

/* 
 * XXX Forward declarations for previously exported functions to avoid moving 
 * things around needlessly 
 */ 
static const char * fileStageString(fileStage a);
static const char * fileActionString(rpmFileAction a);
static int fsmStage(FSM_t fsm, fileStage stage);

/** \ingroup payload
 * Build path to file from file info, ornamented with subdir and suffix.
 * @param fsm		file state machine data
 * @param st		file stat info
 * @param subdir	subdir to use (NULL disables)
 * @param suffix	suffix to use (NULL disables)
 * @retval		path to file (malloced)
 */
static char * fsmFsPath(const FSM_t fsm,
		const struct stat * st,
		const char * subdir,
		const char * suffix)
{
    char * s = NULL;

    if (fsm) {
	int isDir = (st && S_ISDIR(st->st_mode));
	s = rstrscat(NULL, fsm->dirName,
			   (!isDir && subdir) ? subdir : "",
			   fsm->baseName,
			   (!isDir && suffix) ? suffix : "",
			   NULL);
    }
    return s;
}

/** \ingroup payload
 * Destroy file info iterator.
 * @param p		file info iterator
 * @retval		NULL always
 */
static FSMI_t mapFreeIterator(FSMI_t iter)
{
    if (iter) {
	iter->ts = rpmtsFree(iter->ts);
	iter->te = NULL; /* XXX rpmte is not refcounted yet */
	iter->fi = rpmfiFree(iter->fi);
	free(iter);
    }
    return NULL;
}

/** \ingroup payload
 * Create file info iterator.
 * @param ts		transaction set
 * @param fi		transaction element file info
 * @return		file info iterator
 */
static FSMI_t 
mapInitIterator(rpmts ts, rpmte te, rpmfi fi)
{
    FSMI_t iter = NULL;

    iter = xcalloc(1, sizeof(*iter));
    iter->ts = rpmtsLink(ts);
    iter->te = te; /* XXX rpmte is not refcounted yet */
    iter->fi = rpmfiLink(fi);
    iter->reverse = (rpmteType(te) == TR_REMOVED);
    iter->i = (iter->reverse ? (rpmfiFC(fi) - 1) : 0);
    iter->isave = iter->i;
    return iter;
}

/** \ingroup payload
 * Return next index into file info.
 * @param a		file info iterator
 * @return		next index, -1 on termination
 */
static int mapNextIterator(FSMI_t iter)
{
    int i = -1;

    if (iter) {
	const rpmfi fi = iter->fi;
	if (iter->reverse) {
	    if (iter->i >= 0)	i = iter->i--;
	} else {
    	    if (iter->i < rpmfiFC(fi))	i = iter->i++;
	}
	iter->isave = i;
    }
    return i;
}

/** \ingroup payload
 */
static int cpioStrCmp(const void * a, const void * b)
{
    const char * afn = *(const char **)a;
    const char * bfn = *(const char **)b;

    /* Match rpm-4.0 payloads with ./ prefixes. */
    if (afn[0] == '.' && afn[1] == '/')	afn += 2;
    if (bfn[0] == '.' && bfn[1] == '/')	bfn += 2;

    /* If either path is absolute, make it relative. */
    if (afn[0] == '/')	afn += 1;
    if (bfn[0] == '/')	bfn += 1;

    return strcmp(afn, bfn);
}

/** \ingroup payload
 * Locate archive path in file info.
 * @param iter		file info iterator
 * @param fsmPath	archive path
 * @return		index into file info, -1 if archive path was not found
 */
static int mapFind(FSMI_t iter, const char * fsmPath)
{
    int ix = -1;

    if (iter) {
	const rpmfi fi = iter->fi;
	int fc = rpmfiFC(fi);
	if (fi && fc > 0 && fi->apath && fsmPath && *fsmPath) {
	    char ** p = NULL;

	    if (fi->apath != NULL)
		p = bsearch(&fsmPath, fi->apath, fc, sizeof(fsmPath),
			cpioStrCmp);
	    if (p) {
		iter->i = p - fi->apath;
		ix = mapNextIterator(iter);
	    }
	}
    }
    return ix;
}

/** \ingroup payload
 * Directory name iterator.
 */
typedef struct dnli_s {
    rpmfi fi;
    char * active;
    int reverse;
    int isave;
    int i;
} * DNLI_t;

/** \ingroup payload
 * Destroy directory name iterator.
 * @param a		directory name iterator
 * @retval		NULL always
 */
static DNLI_t dnlFreeIterator(DNLI_t dnli)
{
    if (dnli) {
	if (dnli->active) free(dnli->active);
	free(dnli);
    }
    return NULL;
}

/** \ingroup payload
 */
static inline int dnlCount(const DNLI_t dnli)
{
    return (dnli ? rpmfiDC(dnli->fi) : 0);
}

/** \ingroup payload
 */
static inline int dnlIndex(const DNLI_t dnli)
{
    return (dnli ? dnli->isave : -1);
}

/** \ingroup payload
 * Create directory name iterator.
 * @param fsm		file state machine data
 * @param reverse	traverse directory names in reverse order?
 * @return		directory name iterator
 */
static DNLI_t dnlInitIterator(const FSM_t fsm, int reverse)
{
    rpmfi fi = fsmGetFi(fsm);
    rpmfs fs = rpmteGetFileStates(fsmGetTe(fsm));
    DNLI_t dnli;
    int i, j;
    int dc;

    if (fi == NULL)
	return NULL;
    dc = rpmfiDC(fi);
    dnli = xcalloc(1, sizeof(*dnli));
    dnli->fi = fi;
    dnli->reverse = reverse;
    dnli->i = (reverse ? dc : 0);

    if (dc) {
	dnli->active = xcalloc(dc, sizeof(*dnli->active));
	int fc = rpmfiFC(fi);

	/* Identify parent directories not skipped. */
	for (i = 0; i < fc; i++)
            if (!XFA_SKIPPING(rpmfsGetAction(fs, i)))
		dnli->active[rpmfiDIIndex(fi, i)] = 1;

	/* Exclude parent directories that are explicitly included. */
	for (i = 0; i < fc; i++) {
	    int dil;
	    size_t dnlen, bnlen;

	    if (!S_ISDIR(rpmfiFModeIndex(fi, i)))
		continue;

	    dil = rpmfiDIIndex(fi, i);
	    dnlen = strlen(rpmfiDNIndex(fi, dil));
	    bnlen = strlen(rpmfiBNIndex(fi, i));

	    for (j = 0; j < dc; j++) {
		const char * dnl;
		size_t jlen;

		if (!dnli->active[j] || j == dil)
		    continue;
		dnl = rpmfiDNIndex(fi, j);
		jlen = strlen(dnl);
		if (jlen != (dnlen+bnlen+1))
		    continue;
		if (!rstreqn(dnl, rpmfiDNIndex(fi, dil), dnlen))
		    continue;
		if (!rstreqn(dnl+dnlen, rpmfiBNIndex(fi, i), bnlen))
		    continue;
		if (dnl[dnlen+bnlen] != '/' || dnl[dnlen+bnlen+1] != '\0')
		    continue;
		/* This directory is included in the package. */
		dnli->active[j] = 0;
		break;
	    }
	}

	/* Print only once per package. */
	if (!reverse) {
	    j = 0;
	    for (i = 0; i < dc; i++) {
		if (!dnli->active[i]) continue;
		if (j == 0) {
		    j = 1;
		    rpmlog(RPMLOG_DEBUG,
	"========== Directories not explicitly included in package:\n");
		}
		rpmlog(RPMLOG_DEBUG, "%10d %s\n", i, rpmfiDNIndex(fi, i));
	    }
	    if (j)
		rpmlog(RPMLOG_DEBUG, "==========\n");
	}
    }
    return dnli;
}

/** \ingroup payload
 * Return next directory name (from file info).
 * @param dnli		directory name iterator
 * @return		next directory name
 */
static
const char * dnlNextIterator(DNLI_t dnli)
{
    const char * dn = NULL;

    if (dnli) {
	rpmfi fi = dnli->fi;
	int dc = rpmfiDC(fi);
	int i = -1;

	if (dnli->active)
	do {
	    i = (!dnli->reverse ? dnli->i++ : --dnli->i);
	} while (i >= 0 && i < dc && !dnli->active[i]);

	if (i >= 0 && i < dc)
	    dn = rpmfiDNIndex(fi, i);
	else
	    i = -1;
	dnli->isave = i;
    }
    return dn;
}

int fsmNext(FSM_t fsm, fileStage nstage)
{
    fsm->nstage = nstage;
    return fsmStage(fsm, fsm->nstage);
}

/**
 * Map next file path and action.
 * @param fsm		file state machine
 */
static int fsmMapPath(FSM_t fsm)
{
    rpmfi fi = fsmGetFi(fsm);	/* XXX const except for fstates */
    int rc = 0;
    int i;

    fsm->osuffix = NULL;
    fsm->nsuffix = NULL;
    fsm->action = FA_UNKNOWN;

    i = fsm->ix;
    if (fi && i >= 0 && i < rpmfiFC(fi)) {
	rpmte te = fsmGetTe(fsm);
	rpmfs fs = rpmteGetFileStates(te);
	/* XXX these should use rpmfiFFlags() etc */
	fsm->action = rpmfsGetAction(fs, i);
	fsm->fflags = rpmfiFFlagsIndex(fi, i);

	/* src rpms have simple base name in payload. */
	fsm->dirName = rpmfiDNIndex(fi, rpmfiDIIndex(fi, i));
	fsm->baseName = rpmfiBNIndex(fi, i);

        if (rpmteType(te) == TR_ADDED) {
            switch (fsm->action) {
            case FA_SKIPNSTATE:
		rpmfsSetState(fs, i, RPMFILE_STATE_NOTINSTALLED);
                break;
            case FA_SKIPNETSHARED:
		rpmfsSetState(fs, i, RPMFILE_STATE_NETSHARED);
                break;
            case FA_SKIPCOLOR:
		rpmfsSetState(fs, i, RPMFILE_STATE_WRONGCOLOR);
                break;
            case FA_ALTNAME:
                if (!(fsm->fflags & RPMFILE_GHOST)) /* XXX Don't if %ghost file. */
                    fsm->nsuffix = SUFFIX_RPMNEW;
                break;
            case FA_SAVE:
                if (!(fsm->fflags & RPMFILE_GHOST)) /* XXX Don't if %ghost file. */
                    fsm->osuffix = SUFFIX_RPMSAVE;
                break;
            default:
                break;
            }
        }

        if (fsm->action == FA_BACKUP && !(fsm->fflags & RPMFILE_GHOST)) {
            /* XXX Don't if %ghost file. */
            fsm->osuffix = (rpmteType(te) == TR_ADDED) ? SUFFIX_RPMORIG : SUFFIX_RPMSAVE;
        }

	if ((fsm->mapFlags & CPIO_MAP_PATH) || fsm->nsuffix) {
	    const struct stat * st = &fsm->sb;
	    fsm->path = _free(fsm->path);
	    fsm->path = fsmFsPath(fsm, st, NULL,
		(fsm->suffix ? fsm->suffix : fsm->nsuffix));
	}
    }
    return rc;
}

/** \ingroup payload
 * Save hard link in chain.
 * @param fsm		file state machine data
 * @return		Is chain only partially filled?
 */
static int saveHardLink(FSM_t fsm)
{
    struct stat * st = &fsm->sb;
    int rc = 0;
    int ix = -1;
    int j;
    hardLink_t *tailp;

    /* Find hard link set. */
    for (tailp = &fsm->links; (fsm->li = *tailp) != NULL; tailp = &fsm->li->next) {
	if (fsm->li->sb.st_ino == st->st_ino && fsm->li->sb.st_dev == st->st_dev)
	    break;
    }

    /* New hard link encountered, add new link to set. */
    if (fsm->li == NULL) {
	fsm->li = xcalloc(1, sizeof(*fsm->li));
	fsm->li->next = NULL;
	fsm->li->sb = *st;	/* structure assignment */
	fsm->li->nlink = st->st_nlink;
	fsm->li->linkIndex = fsm->ix;
	fsm->li->createdPath = -1;

	fsm->li->filex = xcalloc(st->st_nlink, sizeof(fsm->li->filex[0]));
	memset(fsm->li->filex, -1, (st->st_nlink * sizeof(fsm->li->filex[0])));
	fsm->li->nsuffix = xcalloc(st->st_nlink, sizeof(*fsm->li->nsuffix));

	if (fsm->goal == FSM_PKGBUILD)
	    fsm->li->linksLeft = st->st_nlink;
	if (fsm->goal == FSM_PKGINSTALL)
	    fsm->li->linksLeft = 0;

	*tailp = fsm->li;	/* append to tail of linked list */
    }

    if (fsm->goal == FSM_PKGBUILD) --fsm->li->linksLeft;
    fsm->li->filex[fsm->li->linksLeft] = fsm->ix;
    fsm->li->nsuffix[fsm->li->linksLeft] = fsm->nsuffix;
    if (fsm->goal == FSM_PKGINSTALL) fsm->li->linksLeft++;

    if (fsm->goal == FSM_PKGBUILD)
	return (fsm->li->linksLeft > 0);

    if (fsm->goal != FSM_PKGINSTALL)
	return 0;

    if (!(st->st_size || fsm->li->linksLeft == st->st_nlink))
	return 1;

    /* Here come the bits, time to choose a non-skipped file name. */
    {	rpmfs fs = rpmteGetFileStates(fsmGetTe(fsm));

	for (j = fsm->li->linksLeft - 1; j >= 0; j--) {
	    ix = fsm->li->filex[j];
	    if (ix < 0 || XFA_SKIPPING(rpmfsGetAction(fs, ix)))
		continue;
	    break;
	}
    }

    /* Are all links skipped or not encountered yet? */
    if (ix < 0 || j < 0)
	return 1;	/* XXX W2DO? */

    /* Save the non-skipped file name and map index. */
    fsm->li->linkIndex = j;
    fsm->path = _free(fsm->path);
    fsm->ix = ix;
    rc = fsmMapPath(fsm);
    return rc;
}

/** \ingroup payload
 * Destroy set of hard links.
 * @param li		set of hard links
 * @return		NULL always
 */
static hardLink_t freeHardLink(hardLink_t li)
{
    if (li) {
	li->nsuffix = _free(li->nsuffix);	/* XXX elements are shared */
	li->filex = _free(li->filex);
	_free(li);
    }
    return NULL;
}

/* forward declaration*/
static int fsmMkdirs(DNLI_t dnli, struct selabel_handle *sehandle);

static int fsmCreate(FSM_t fsm)
{
    int rc = 0;
    fsm->path = _free(fsm->path);

    fsm->rdsize = fsm->wrsize = 0;
    fsm->rdbuf = fsm->rdb = _free(fsm->rdb);
    fsm->wrbuf = fsm->wrb = _free(fsm->wrb);
    if (fsm->goal == FSM_PKGINSTALL || fsm->goal == FSM_PKGBUILD) {
        fsm->rdsize = 8 * BUFSIZ;
        fsm->rdbuf = fsm->rdb = xmalloc(fsm->rdsize);
        fsm->wrsize = 8 * BUFSIZ;
        fsm->wrbuf = fsm->wrb = xmalloc(fsm->wrsize);
    }

    fsm->ix = -1;
    fsm->links = NULL;
    fsm->li = NULL;
    errno = 0;	/* XXX get rid of EBADF */

    /* Detect and create directories not explicitly in package. */
    /* XXX This seems like a strange place to do this... */
    if (fsm->goal == FSM_PKGINSTALL) {
	DNLI_t dnli = dnlInitIterator(fsm, 0);
	rc = fsmMkdirs(dnli, fsm->sehandle);
	dnlFreeIterator(dnli);	
    }
    return rc;
}

static int fsmSetup(FSM_t fsm, fileStage goal,
		rpmts ts, rpmte te, rpmfi fi, FD_t cfd, rpmpsm psm,
		rpm_loff_t * archiveSize, char ** failedFile)
{
    int rc, ec = 0;
    int isSrc = rpmteIsSource(te);

    fsm->goal = goal;
    if (cfd != NULL) {
	fsm->cfd = fdLink(cfd);
    }
    fsm->cpioPos = 0;
    fsm->iter = mapInitIterator(ts, te, fi);
    fsm->psm = psm;
    fsm->sehandle = rpmtsSELabelHandle(ts);

    fsm->mapFlags = CPIO_MAP_PATH | CPIO_MAP_MODE | CPIO_MAP_UID | CPIO_MAP_GID;
    if (goal == FSM_PKGBUILD) {
	fsm->mapFlags |= CPIO_MAP_TYPE;
	if (isSrc) {
	    fsm->mapFlags |= CPIO_FOLLOW_SYMLINKS;
	}
    } else {
	if (!isSrc) {
	    fsm->mapFlags |= CPIO_SBIT_CHECK;
	}
    }

    fsm->archiveSize = archiveSize;
    if (fsm->archiveSize)
	*fsm->archiveSize = 0;
    fsm->failedFile = failedFile;
    if (fsm->failedFile)
	*fsm->failedFile = NULL;

    if (fsm->goal == FSM_PKGINSTALL) {
	rasprintf(&fsm->suffix, ";%08x", (unsigned)rpmtsGetTid(ts));
    }

    ec = fsm->rc = 0;
    rc = fsmCreate(fsm);
    if (rc && !ec) ec = rc;

    rc = fsmUNSAFE(fsm, fsm->goal);
    if (rc && !ec) ec = rc;

    if (fsm->archiveSize && ec == 0)
	*fsm->archiveSize = fsm->cpioPos;

/* FIX: *fsm->failedFile may be NULL */
   return ec;
}

static int fsmTeardown(FSM_t fsm)
{
    int rc = fsm->rc;

    if (!rc)
	rc = fsmUNSAFE(fsm, FSM_DESTROY);

    fsm->iter = mapFreeIterator(fsm->iter);
    if (fsm->cfd != NULL) {
	fsm->cfd = fdFree(fsm->cfd);
	fsm->cfd = NULL;
    }
    fsm->failedFile = NULL;

    fsm->path = _free(fsm->path);
    fsm->suffix = _free(fsm->suffix);
    while ((fsm->li = fsm->links) != NULL) {
	fsm->links = fsm->li->next;
	fsm->li->next = NULL;
	fsm->li = freeHardLink(fsm->li);
    }
    return rc;
}

/* Find and set file security context */
static int fsmSetSELabel(struct selabel_handle *sehandle,
			 const char *path, mode_t mode)
{
    int rc = 0;
#if WITH_SELINUX
    if (sehandle) {
	security_context_t scon = NULL;

	if (selabel_lookup_raw(sehandle, &scon, path, mode) == 0) {
	    rc = lsetfilecon(path, scon);

	    if (_fsm_debug && (FSM_LSETFCON & FSM_SYSCALL)) {
		rpmlog(RPMLOG_DEBUG, " %8s (%s, %s) %s\n",
			fileStageString(FSM_LSETFCON), path, scon,
			(rc < 0 ? strerror(errno) : ""));
	    }

	    if (rc < 0 && errno == EOPNOTSUPP)
		rc = 0;
	}

	freecon(scon);
    }
#endif
    return rc ? CPIOERR_LSETFCON_FAILED : 0;
}

static int fsmSetFCaps(const char *path, const char *captxt)
{
    int rc = 0;
#if WITH_CAP
    if (captxt && *captxt != '\0') {
	cap_t fcaps = cap_from_text(captxt);
	if (fcaps == NULL || cap_set_file(path, fcaps) != 0) {
	    rc = CPIOERR_SETCAP_FAILED;
	}
	cap_free(fcaps);
    } 
#endif
    return rc;
}

/**
 * Map file stat(2) info.
 * @param fsm		file state machine
 */
static int fsmMapAttrs(FSM_t fsm)
{
    struct stat * st = &fsm->sb;
    rpmfi fi = fsmGetFi(fsm);
    int i = fsm->ix;

    /* this check is pretty moot,  rpmfi accessors check array bounds etc */
    if (fi && i >= 0 && i < rpmfiFC(fi)) {
	ino_t finalInode = rpmfiFInodeIndex(fi, i);
	mode_t finalMode = rpmfiFModeIndex(fi, i);
	dev_t finalRdev = rpmfiFRdevIndex(fi, i);
	time_t finalMtime = rpmfiFMtimeIndex(fi, i);
	const char *user = rpmfiFUserIndex(fi, i);
	const char *group = rpmfiFGroupIndex(fi, i);
	uid_t uid = 0;
	gid_t gid = 0;

	if (user && rpmugUid(user, &uid)) {
	    if (fsm->goal == FSM_PKGINSTALL)
		rpmlog(RPMLOG_WARNING,
		    _("user %s does not exist - using root\n"), user);
	    finalMode &= ~S_ISUID;      /* turn off suid bit */
	}

	if (group && rpmugGid(group, &gid)) {
	    if (fsm->goal == FSM_PKGINSTALL)
		rpmlog(RPMLOG_WARNING,
		    _("group %s does not exist - using root\n"), group);
	    finalMode &= ~S_ISGID;	/* turn off sgid bit */
	}

	if (fsm->mapFlags & CPIO_MAP_MODE)
	    st->st_mode = (st->st_mode & S_IFMT) | (finalMode & ~S_IFMT);
	if (fsm->mapFlags & CPIO_MAP_TYPE) {
	    st->st_mode = (st->st_mode & ~S_IFMT) | (finalMode & S_IFMT);
	    if ((S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
	    && st->st_nlink == 0)
		st->st_nlink = 1;
	    st->st_ino = finalInode;
	    st->st_rdev = finalRdev;
	    st->st_mtime = finalMtime;
	}
	if (fsm->mapFlags & CPIO_MAP_UID)
	    st->st_uid = uid;
	if (fsm->mapFlags & CPIO_MAP_GID)
	    st->st_gid = gid;
    }
    return 0;
}

/** \ingroup payload
 * Create file from payload stream.
 * @param fsm		file state machine data
 * @return		0 on success
 */
static int expandRegular(FSM_t fsm)
{
    FD_t wfd = NULL;
    const struct stat * st = &fsm->sb;
    rpm_loff_t left = st->st_size;
    const unsigned char * fidigest = NULL;
    pgpHashAlgo digestalgo = 0;
    int rc = 0;

    wfd = Fopen(fsm->path, "w.ufdio");
    if (Ferror(wfd)) {
	rc = CPIOERR_OPEN_FAILED;
	goto exit;
    }

    if (!(rpmtsFlags(fsmGetTs(fsm)) & RPMTRANS_FLAG_NOFILEDIGEST)) {
	rpmfi fi = fsmGetFi(fsm);
	digestalgo = rpmfiDigestAlgo(fi);
	fidigest = rpmfiFDigestIndex(fi, fsm->ix, NULL, NULL);
    }

    if (st->st_size > 0 && fidigest)
	fdInitDigest(wfd, digestalgo, 0);

    while (left) {

	fsm->wrlen = (left > fsm->wrsize ? fsm->wrsize : left);
	rc = fsmNext(fsm, FSM_DREAD);
	if (rc)
	    goto exit;

	fsm->wrnb = Fwrite(fsm->wrbuf, sizeof(*fsm->wrbuf), fsm->rdnb, wfd);
	if (fsm->rdnb != fsm->wrnb || Ferror(wfd)) {
	    rc = CPIOERR_WRITE_FAILED;
	    goto exit;
	}

	left -= fsm->wrnb;

	/* don't call this with fileSize == fileComplete */
	if (!rc && left)
	    (void) fsmNext(fsm, FSM_NOTIFY);
    }

    if (st->st_size > 0 && fidigest) {
	void * digest = NULL;

	(void) Fflush(wfd);
	fdFiniDigest(wfd, digestalgo, &digest, NULL, 0);

	if (digest != NULL && fidigest != NULL) {
	    size_t diglen = rpmDigestLength(digestalgo);
	    if (memcmp(digest, fidigest, diglen))
		rc = CPIOERR_DIGEST_MISMATCH;
	} else {
	    rc = CPIOERR_DIGEST_MISMATCH;
	}
	free(digest);
    }

exit:
    if (wfd) {
	int myerrno = errno;
	rpmswAdd(rpmtsOp(fsmGetTs(fsm), RPMTS_OP_DIGEST),
		 fdOp(wfd, FDSTAT_DIGEST));
	Fclose(wfd);
	errno = myerrno;
    }
    return rc;
}

static int fsmReadLink(const char *path,
		       char *buf, size_t bufsize, size_t *linklen)
{
    ssize_t llen = readlink(path, buf, bufsize - 1);
    int rc = CPIOERR_READLINK_FAILED;

    if (_fsm_debug && (FSM_READLINK & FSM_SYSCALL)) {
        rpmlog(RPMLOG_DEBUG, " %8s (%s, rdbuf, %d) %s\n",
	       fileStageString(FSM_READLINK),
               path, (int)(bufsize -1), (llen < 0 ? strerror(errno) : ""));
    }

    if (llen >= 0) {
	buf[llen] = '\0';
	rc = 0;
	*linklen = llen;
    }
    return rc;
}

/** \ingroup payload
 * Write next item to payload stream.
 * @param fsm		file state machine data
 * @param writeData	should data be written?
 * @return		0 on success
 */
static int writeFile(FSM_t fsm, int writeData)
{
    FD_t rfd = NULL;
    char * path = fsm->path;
    struct stat * st = &fsm->sb;
    struct stat * ost = &fsm->osb;
    char * symbuf = NULL;
    rpm_loff_t left;
    int rc;

    st->st_size = (writeData ? ost->st_size : 0);

    if (S_ISDIR(st->st_mode)) {
	st->st_size = 0;
    } else if (S_ISLNK(st->st_mode)) {
	/*
	 * While linux puts the size of a symlink in the st_size field,
	 * I don't think that's a specified standard.
	 */
	/* XXX NUL terminated result in fsm->rdbuf, len in fsm->rdnb. */
	rc = fsmReadLink(fsm->path, fsm->rdbuf, fsm->rdsize, &fsm->rdnb);
	if (rc) goto exit;
	st->st_size = fsm->rdnb;
	rstrcat(&symbuf, fsm->rdbuf);	/* XXX save readlink return. */
    }

    if (fsm->mapFlags & CPIO_MAP_ABSOLUTE) {
	fsm->path = rstrscat(NULL, (fsm->mapFlags & CPIO_MAP_ADDDOT) ? "." : "",
				   fsm->dirName, fsm->baseName, NULL);
    } else if (fsm->mapFlags & CPIO_MAP_PATH) {
	rpmfi fi = fsmGetFi(fsm);
	fsm->path = xstrdup((fi->apath ? fi->apath[fsm->ix] : 
					 rpmfiBNIndex(fi, fsm->ix)));
    }

    rc = cpioHeaderWrite(fsm, st);
    _free(fsm->path);
    fsm->path = path;
    if (rc) goto exit;

    if (writeData && S_ISREG(st->st_mode)) {
	size_t rdlen;
#ifdef HAVE_MMAP
	char * rdbuf = NULL;
	void * mapped = MAP_FAILED;
	size_t nmapped;
	int xx;
#endif

	rfd = Fopen(fsm->path, "r.ufdio");
	if (Ferror(rfd)) {
	    rc = CPIOERR_OPEN_FAILED;
	    goto exit;
	}
	
	/* XXX unbuffered mmap generates *lots* of fdio debugging */
#ifdef HAVE_MMAP
	nmapped = 0;
	mapped = mmap(NULL, st->st_size, PROT_READ, MAP_SHARED, Fileno(rfd), 0);
	if (mapped != MAP_FAILED) {
	    rdbuf = fsm->rdbuf;
	    fsm->rdbuf = (char *) mapped;
	    rdlen = nmapped = st->st_size;
#if defined(MADV_DONTNEED)
	    xx = madvise(mapped, nmapped, MADV_DONTNEED);
#endif
	}
#endif

	left = st->st_size;

	while (left) {
#ifdef HAVE_MMAP
	  if (mapped != MAP_FAILED) {
	    fsm->rdnb = nmapped;
	  } else
#endif
	  {
	    rdlen = (left > fsm->rdsize ? fsm->rdsize : left),

	    fsm->rdnb = Fread(fsm->rdbuf, sizeof(*fsm->rdbuf), rdlen, rfd);
	    if (fsm->rdnb != rdlen || Ferror(rfd)) {
		rc = CPIOERR_READ_FAILED;
		goto exit;
	    }
	  }

	    /* XXX DWRITE uses rdnb for I/O length. */
	    rc = fsmNext(fsm, FSM_DWRITE);
	    if (rc) goto exit;

	    left -= fsm->wrnb;
	}

#ifdef HAVE_MMAP
	if (mapped != MAP_FAILED) {
	    xx = msync(mapped, nmapped, MS_ASYNC);
#if defined(MADV_DONTNEED)
	    xx = madvise(mapped, nmapped, MADV_DONTNEED);
#endif
	    xx = munmap(mapped, nmapped);
	    fsm->rdbuf = rdbuf;
	}
#endif

    } else if (writeData && S_ISLNK(st->st_mode)) {
	/* XXX DWRITE uses rdnb for I/O length. */
	strcpy(fsm->rdbuf, symbuf);	/* XXX restore readlink buffer. */
	fsm->rdnb = strlen(symbuf);
	rc = fsmNext(fsm, FSM_DWRITE);
	if (rc) goto exit;
    }

    rc = fsmNext(fsm, FSM_PAD);
    if (rc) goto exit;

    rc = 0;

exit:
    if (rfd) {
	/* preserve any prior errno across close */
	int myerrno = errno;
	rpmswAdd(rpmtsOp(fsmGetTs(fsm), RPMTS_OP_DIGEST),
		 fdOp(rfd, FDSTAT_DIGEST));
	Fclose(rfd);
	errno = myerrno;
    }
    fsm->path = path;
    free(symbuf);
    return rc;
}

/** \ingroup payload
 * Write set of linked files to payload stream.
 * @param fsm		file state machine data
 * @return		0 on success
 */
static int writeLinkedFile(FSM_t fsm)
{
    char * path = fsm->path;
    const char * nsuffix = fsm->nsuffix;
    int iterIndex = fsm->ix;
    int ec = 0;
    int rc;
    int i;

    fsm->path = NULL;
    fsm->nsuffix = NULL;
    fsm->ix = -1;

    for (i = fsm->li->nlink - 1; i >= 0; i--) {

	if (fsm->li->filex[i] < 0) continue;

	fsm->ix = fsm->li->filex[i];
	rc = fsmMapPath(fsm);

	/* Write data after last link. */
	rc = writeFile(fsm, (i == 0));
	if (fsm->failedFile && rc != 0 && *fsm->failedFile == NULL) {
	    ec = rc;
	    *fsm->failedFile = xstrdup(fsm->path);
	}

	fsm->path = _free(fsm->path);
	fsm->li->filex[i] = -1;
    }

    fsm->ix = iterIndex;
    fsm->nsuffix = nsuffix;
    fsm->path = path;
    return ec;
}

static int writeLinks(FSM_t fsm)
{
    int j, rc = 0;
    nlink_t i, nlink;

    while ((fsm->li = fsm->links) != NULL) {
	fsm->links = fsm->li->next;
	fsm->li->next = NULL;

	/* Re-calculate link count for archive header. */
	for (j = -1, nlink = 0, i = 0; i < fsm->li->nlink; i++) {
	    if (fsm->li->filex[i] < 0)
		continue;
	    nlink++;
	    if (j == -1) j = i;
	}
	/* XXX force the contents out as well. */
	if (j != 0) {
	    fsm->li->filex[0] = fsm->li->filex[j];
	    fsm->li->filex[j] = -1;
	}
	fsm->li->sb.st_nlink = nlink;

	fsm->sb = fsm->li->sb;	/* structure assignment */
	fsm->osb = fsm->sb;	/* structure assignment */

	if (!rc) rc = writeLinkedFile(fsm);

	fsm->li = freeHardLink(fsm->li);
    }
    return rc;
}

static int fsmStat(const char *path, int dolstat, struct stat *sb)
{
    int rc;
    if (dolstat){
	rc = lstat(path, sb);
    } else {
        rc = stat(path, sb);
    }
    if (_fsm_debug && (FSM_STAT & FSM_SYSCALL) && rc && errno != ENOENT)
        rpmlog(RPMLOG_DEBUG, " %8s (%s, ost) %s\n",
               fileStageString(dolstat ? FSM_LSTAT : FSM_STAT),
               path, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0) {
        rc = (errno == ENOENT ? CPIOERR_ENOENT : CPIOERR_LSTAT_FAILED);
	/* WTH is this, and is it really needed, still? */
        memset(sb, 0, sizeof(*sb));	/* XXX s390x hackery */
    }
    return rc;
}

static int fsmVerify(FSM_t fsm);

/** \ingroup payload
 * Create pending hard links to existing file.
 * @param fsm		file state machine data
 * @return		0 on success
 */
static int fsmMakeLinks(FSM_t fsm)
{
    char * path = fsm->path;
    char * opath = NULL;
    const char * nsuffix = fsm->nsuffix;
    int iterIndex = fsm->ix;
    int ec = 0;
    int rc;
    int i;

    fsm->path = NULL;
    fsm->nsuffix = NULL;
    fsm->ix = -1;

    fsm->ix = fsm->li->filex[fsm->li->createdPath];
    rc = fsmMapPath(fsm);
    opath = fsm->path;
    fsm->path = NULL;
    for (i = 0; i < fsm->li->nlink; i++) {
	if (fsm->li->filex[i] < 0) continue;
	if (fsm->li->createdPath == i) continue;

	fsm->ix = fsm->li->filex[i];
	fsm->path = _free(fsm->path);
	rc = fsmMapPath(fsm);
	if (XFA_SKIPPING(fsm->action)) continue;

	rc = fsmVerify(fsm);
	if (!rc) continue;
	if (!(rc == CPIOERR_ENOENT)) break;

	/* XXX link(opath, fsm->path) */
	rc = link(opath, fsm->path);
	if (_fsm_debug && (FSM_LINK & FSM_SYSCALL))
	    rpmlog(RPMLOG_DEBUG, " %8s (%s, %s) %s\n", fileStageString(FSM_LINK),
		opath, fsm->path, (rc < 0 ? strerror(errno) : ""));
	if (rc < 0)	rc = CPIOERR_LINK_FAILED;

	if (fsm->failedFile && rc != 0 && *fsm->failedFile == NULL) {
	    ec = rc;
	    *fsm->failedFile = xstrdup(fsm->path);
	}

	fsm->li->linksLeft--;
    }
    fsm->path = _free(fsm->path);
    free(opath);

    fsm->ix = iterIndex;
    fsm->nsuffix = nsuffix;
    fsm->path = path;
    return ec;
}

/** \ingroup payload
 * Commit hard linked file set atomically.
 * @param fsm		file state machine data
 * @return		0 on success
 */
static int fsmCommitLinks(FSM_t fsm)
{
    char * path = fsm->path;
    const char * nsuffix = fsm->nsuffix;
    int iterIndex = fsm->ix;
    struct stat * st = &fsm->sb;
    int rc = 0;
    nlink_t i;

    fsm->path = NULL;
    fsm->nsuffix = NULL;
    fsm->ix = -1;

    for (fsm->li = fsm->links; fsm->li; fsm->li = fsm->li->next) {
	if (fsm->li->sb.st_ino == st->st_ino && fsm->li->sb.st_dev == st->st_dev)
	    break;
    }

    for (i = 0; i < fsm->li->nlink; i++) {
	if (fsm->li->filex[i] < 0) continue;
	fsm->ix = fsm->li->filex[i];
	rc = fsmMapPath(fsm);
	if (!XFA_SKIPPING(fsm->action))
	    rc = fsmNext(fsm, FSM_COMMIT);
	fsm->path = _free(fsm->path);
	fsm->li->filex[i] = -1;
    }

    fsm->ix = iterIndex;
    fsm->nsuffix = nsuffix;
    fsm->path = path;
    return rc;
}

static int fsmRmdir(const char *path)
{
    int rc = rmdir(path);
    if (_fsm_debug && (FSM_RMDIR & FSM_SYSCALL))
	rpmlog(RPMLOG_DEBUG, " %8s (%s) %s\n", fileStageString(FSM_RMDIR),
	       path, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)
	switch (errno) {
	case ENOENT:        rc = CPIOERR_ENOENT;    break;
	case ENOTEMPTY:     rc = CPIOERR_ENOTEMPTY; break;
	default:            rc = CPIOERR_RMDIR_FAILED; break;
	}
    return rc;
}

static int fsmMkdir(const char *path, mode_t mode)
{
    int rc = mkdir(path, (mode & 07777));
    if (_fsm_debug && (FSM_MKDIR & FSM_SYSCALL))
	rpmlog(RPMLOG_DEBUG, " %8s (%s, 0%04o) %s\n", fileStageString(FSM_MKDIR),
	       path, (unsigned)(mode & 07777),
	       (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = CPIOERR_MKDIR_FAILED;
    return rc;
}

static int fsmMkfifo(const char *path, mode_t mode)
{
    int rc = mkfifo(path, (mode & 07777));

    if (_fsm_debug && (FSM_MKFIFO & FSM_SYSCALL)) {
	rpmlog(RPMLOG_DEBUG, " %8s (%s, 0%04o) %s\n",
	       fileStageString(FSM_MKFIFO), path, (unsigned)(mode & 07777),
	       (rc < 0 ? strerror(errno) : ""));
    }

    if (rc < 0)
	rc = CPIOERR_MKFIFO_FAILED;

    return rc;
}

static int fsmMknod(const char *path, mode_t mode, dev_t dev)
{
    /* FIX: check S_IFIFO or dev != 0 */
    int rc = mknod(path, (mode & ~07777), dev);

    if (_fsm_debug && (FSM_MKNOD & FSM_SYSCALL)) {
	rpmlog(RPMLOG_DEBUG, " %8s (%s, 0%o, 0x%x) %s\n",
	       fileStageString(FSM_MKNOD), path, (unsigned)(mode & ~07777),
	       (unsigned)dev, (rc < 0 ? strerror(errno) : ""));
    }

    if (rc < 0)
	rc = CPIOERR_MKNOD_FAILED;

    return rc;
}

/**
 * Create (if necessary) directories not explicitly included in package.
 * @param dnli		file state machine data
 * @param sehandle	selinux label handle (bah)
 * @return		0 on success
 */
static int fsmMkdirs(DNLI_t dnli, struct selabel_handle *sehandle)
{
    struct stat sb;
    const char *dpath;
    int dc = dnlCount(dnli);
    int rc = 0;
    int i;
    int ldnlen = 0;
    int ldnalloc = 0;
    char * ldn = NULL;
    short * dnlx = NULL; 

    dnlx = (dc ? xcalloc(dc, sizeof(*dnlx)) : NULL);

    if (dnlx != NULL)
    while ((dpath = dnlNextIterator(dnli)) != NULL) {
	size_t dnlen = strlen(dpath);
	char * te, dn[dnlen+1];

	dc = dnlIndex(dnli);
	if (dc < 0) continue;
	dnlx[dc] = dnlen;
	if (dnlen <= 1)
	    continue;

	if (dnlen <= ldnlen && rstreq(dpath, ldn))
	    continue;

	/* Copy as we need to modify the string */
	(void) stpcpy(dn, dpath);

	/* Assume '/' directory exists, "mkdir -p" for others if non-existent */
	for (i = 1, te = dn + 1; *te != '\0'; te++, i++) {
	    if (*te != '/')
		continue;

	    *te = '\0';

	    /* Already validated? */
	    if (i < ldnlen &&
		(ldn[i] == '/' || ldn[i] == '\0') && rstreqn(dn, ldn, i))
	    {
		*te = '/';
		/* Move pre-existing path marker forward. */
		dnlx[dc] = (te - dn);
		continue;
	    }

	    /* Validate next component of path. */
	    rc = fsmStat(dn, 1, &sb); /* lstat */
	    *te = '/';

	    /* Directory already exists? */
	    if (rc == 0 && S_ISDIR(sb.st_mode)) {
		/* Move pre-existing path marker forward. */
		dnlx[dc] = (te - dn);
	    } else if (rc == CPIOERR_ENOENT) {
		*te = '\0';
		mode_t mode = S_IFDIR | (_dirPerms & 07777);
		rc = fsmMkdir(dn, mode);
		if (!rc) {
		    rc = fsmSetSELabel(sehandle, dn, mode);

		    rpmlog(RPMLOG_DEBUG,
			    "%s directory created with perms %04o\n",
			    dn, (unsigned)(mode & 07777));
		}
		*te = '/';
	    }
	    if (rc)
		break;
	}
	if (rc) break;

	/* Save last validated path. */
	if (ldnalloc < (dnlen + 1)) {
	    ldnalloc = dnlen + 100;
	    ldn = xrealloc(ldn, ldnalloc);
	}
	if (ldn != NULL) { /* XXX can't happen */
	    strcpy(ldn, dn);
	    ldnlen = dnlen;
	}
    }
    free(dnlx);
    free(ldn);

    return rc;
}

static void removeSBITS(const char *path)
{
    struct stat stb;
    if (lstat(path, &stb) == 0 && S_ISREG(stb.st_mode)) {
	if ((stb.st_mode & 06000) != 0) {
	    (void) chmod(path, stb.st_mode & 0777);
	}
#if WITH_CAP
	if (stb.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) {
	    (void) cap_set_file(path, NULL);
	}
#endif
    }
}

/********************************************************************/

static int fsmInit(FSM_t fsm)
{
    int rc = 0;

    fsm->path = _free(fsm->path);
    fsm->postpone = 0;
    fsm->diskchecked = fsm->exists = 0;
    fsm->action = FA_UNKNOWN;
    fsm->osuffix = NULL;
    fsm->nsuffix = NULL;

    if (fsm->goal == FSM_PKGINSTALL) {
	/* Read next header from payload, checking for end-of-payload. */
	rc = fsmUNSAFE(fsm, FSM_NEXT);
    }
    if (rc) return rc;

    /* Identify mapping index. */
    fsm->ix = ((fsm->goal == FSM_PKGINSTALL)
	       ? mapFind(fsm->iter, fsm->path) : mapNextIterator(fsm->iter));

    /* Detect end-of-loop and/or mapping error. */
    if (fsm->ix < 0) {
	if (fsm->goal == FSM_PKGINSTALL) {
#if 0
	    rpmlog(RPMLOG_WARNING,
		   _("archive file %s was not found in header file list\n"),
		   fsm->path);
#endif
	    if (fsm->failedFile && *fsm->failedFile == NULL)
		*fsm->failedFile = xstrdup(fsm->path);
	    rc = CPIOERR_UNMAPPED_FILE;
	} else {
	    rc = CPIOERR_HDR_TRAILER;
	}
	return rc;
    }

    /* On non-install, mode must be known so that dirs don't get suffix. */
    if (fsm->goal != FSM_PKGINSTALL) {
	rpmfi fi = fsmGetFi(fsm);
	fsm->sb.st_mode = rpmfiFModeIndex(fi, fsm->ix);
    }

    /* Generate file path. */
    rc = fsmMapPath(fsm);
    if (rc) return rc;

    /* Perform lstat/stat for disk file. */
    if (fsm->path != NULL &&
	!(fsm->goal == FSM_PKGINSTALL && S_ISREG(fsm->sb.st_mode)))
    {
	int dolstat = !(fsm->mapFlags & CPIO_FOLLOW_SYMLINKS);
	rc = fsmStat(fsm->path, dolstat, &fsm->osb);
	if (rc == CPIOERR_ENOENT) {
	    // errno = saveerrno; XXX temporary commented out
	    rc = 0;
	    fsm->exists = 0;
	} else if (rc == 0) {
	    fsm->exists = 1;
	}
    } else {
	/* Skip %ghost files on build. */
	fsm->exists = 0;
    }
    fsm->diskchecked = 1;
    if (rc) return rc;

    /* On non-install, the disk file stat is what's remapped. */
    if (fsm->goal != FSM_PKGINSTALL)
	fsm->sb = fsm->osb;			/* structure assignment */

    /* Remap file perms, owner, and group. */
    rc = fsmMapAttrs(fsm);
    if (rc) return rc;

    fsm->postpone = XFA_SKIPPING(fsm->action);
    if (fsm->goal == FSM_PKGINSTALL || fsm->goal == FSM_PKGBUILD) {
	/* FIX: saveHardLink can modify fsm */
	if (S_ISREG(fsm->sb.st_mode) && fsm->sb.st_nlink > 1)
	    fsm->postpone = saveHardLink(fsm);
    }
    return rc;

}

static int fsmSymlink(const char *opath, const char *path)
{
    int rc = symlink(opath, path);

    if (_fsm_debug && (FSM_SYMLINK & FSM_SYSCALL)) {
	rpmlog(RPMLOG_DEBUG, " %8s (%s, %s) %s\n", fileStageString(FSM_SYMLINK),
	       opath, path, (rc < 0 ? strerror(errno) : ""));
    }

    if (rc < 0)
	rc = CPIOERR_SYMLINK_FAILED;
    return rc;
}

static int fsmUnlink(const char *path, cpioMapFlags mapFlags)
{
    int rc = 0;
    if (mapFlags & CPIO_SBIT_CHECK)
        removeSBITS(path);
    rc = unlink(path);
    if (_fsm_debug && (FSM_UNLINK & FSM_SYSCALL))
	rpmlog(RPMLOG_DEBUG, " %8s (%s) %s\n", fileStageString(FSM_UNLINK),
	       path, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)
	rc = (errno == ENOENT ? CPIOERR_ENOENT : CPIOERR_UNLINK_FAILED);
    return rc;
}

static int fsmRename(const char *opath, const char *path,
		     cpioMapFlags mapFlags)
{
    if (mapFlags & CPIO_SBIT_CHECK)
        removeSBITS(path);
    int rc = rename(opath, path);
#if defined(ETXTBSY) && defined(__HPUX__)
    /* XXX HP-UX (and other os'es) don't permit rename to busy files. */
    if (rc && errno == ETXTBSY) {
	char *rmpath = NULL;
	rstrscat(&rmpath, path, "-RPMDELETE", NULL);
	rc = rename(path, rmpath);
	if (!rc) rc = rename(opath, path);
	free(rmpath);
    }
#endif
    if (_fsm_debug && (FSM_RENAME & FSM_SYSCALL))
	rpmlog(RPMLOG_DEBUG, " %8s (%s, %s) %s\n", fileStageString(FSM_RENAME),
	       opath, path, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = CPIOERR_RENAME_FAILED;
    return rc;
}


static int fsmChown(const char *path, uid_t uid, gid_t gid)
{
    int rc = chown(path, uid, gid);
    if (rc < 0) {
	struct stat st;
	if (lstat(path, &st) == 0 && st.st_uid == uid && st.st_gid == gid)
	    rc = 0;
    }
    if (_fsm_debug && (FSM_CHOWN & FSM_SYSCALL))
	rpmlog(RPMLOG_DEBUG, " %8s (%s, %d, %d) %s\n", fileStageString(FSM_CHOWN),
	       path, (int)uid, (int)gid,
	       (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = CPIOERR_CHOWN_FAILED;
    return rc;
}

static int fsmLChown(const char *path, uid_t uid, gid_t gid)
{
    int rc = lchown(path, uid, gid);
    if (rc < 0) {
	struct stat st;
	if (lstat(path, &st) == 0 && st.st_uid == uid && st.st_gid == gid)
	    rc = 0;
    }
    if (_fsm_debug && (FSM_LCHOWN & FSM_SYSCALL))
	rpmlog(RPMLOG_DEBUG, " %8s (%s, %d, %d) %s\n", fileStageString(FSM_LCHOWN),
	       path, (int)uid, (int)gid,
	       (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = CPIOERR_CHOWN_FAILED;
    return rc;
}

static int fsmChmod(const char *path, mode_t mode)
{
    int rc = chmod(path, (mode & 07777));
    if (rc < 0) {
	struct stat st;
	if (lstat(path, &st) == 0 && (st.st_mode & 07777) == (mode & 07777))
	    rc = 0;
    }
    if (_fsm_debug && (FSM_CHMOD & FSM_SYSCALL))
	rpmlog(RPMLOG_DEBUG, " %8s (%s, 0%04o) %s\n", fileStageString(FSM_CHMOD),
	       path, (unsigned)(mode & 07777),
	       (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = CPIOERR_CHMOD_FAILED;
    return rc;
}

static int fsmUtime(const char *path, time_t mtime)
{
    int rc = 0;
    struct utimbuf stamp;
    stamp.actime = mtime;
    stamp.modtime = mtime;
    rc = utime(path, &stamp);
    if (_fsm_debug && (FSM_UTIME & FSM_SYSCALL))
	rpmlog(RPMLOG_DEBUG, " %8s (%s, 0x%x) %s\n", fileStageString(FSM_UTIME),
	       path, (unsigned)mtime, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = CPIOERR_UTIME_FAILED;
    return rc;
}

static int fsmVerify(FSM_t fsm)
{
    int rc;
    struct stat * st = &fsm->sb;
    struct stat * ost = &fsm->osb;
    int saveerrno = errno;

    if (fsm->diskchecked && !fsm->exists) {
        return CPIOERR_ENOENT;
    }
    if (S_ISREG(st->st_mode)) {
	/* HP-UX (and other os'es) don't permit unlink on busy files. */
	char *rmpath = rstrscat(NULL, fsm->path, "-RPMDELETE", NULL);
	rc = fsmRename(fsm->path, rmpath, fsm->mapFlags);
	/* XXX shouldn't we take unlink return code here? */
	if (!rc)
	    (void) fsmUnlink(rmpath, fsm->mapFlags);
	else
	    rc = CPIOERR_UNLINK_FAILED;
	free(rmpath);
        return (rc ? rc : CPIOERR_ENOENT);	/* XXX HACK */
    } else if (S_ISDIR(st->st_mode)) {
        if (S_ISDIR(ost->st_mode)) return 0;
        if (S_ISLNK(ost->st_mode)) {
            rc = fsmStat(fsm->path, 0, &fsm->osb);
            if (rc == CPIOERR_ENOENT) rc = 0;
            if (rc) return rc;
            errno = saveerrno;
            if (S_ISDIR(ost->st_mode)) return 0;
        }
    } else if (S_ISLNK(st->st_mode)) {
        if (S_ISLNK(ost->st_mode)) {
            /* XXX NUL terminated result in fsm->rdbuf, len in fsm->rdnb. */
            rc = fsmReadLink(fsm->path, fsm->rdbuf, fsm->rdsize, &fsm->rdnb);
            errno = saveerrno;
            if (rc) return rc;
	    /* XXX FSM_PROCESS puts link target to wrbuf. */
            if (rstreq(fsm->wrbuf, fsm->rdbuf))	return 0;
        }
    } else if (S_ISFIFO(st->st_mode)) {
        if (S_ISFIFO(ost->st_mode)) return 0;
    } else if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) {
        if ((S_ISCHR(ost->st_mode) || S_ISBLK(ost->st_mode)) &&
            (ost->st_rdev == st->st_rdev)) return 0;
    } else if (S_ISSOCK(st->st_mode)) {
        if (S_ISSOCK(ost->st_mode)) return 0;
    }
    /* XXX shouldn't do this with commit/undo. */
    rc = 0;
    if (fsm->stage == FSM_PROCESS) rc = fsmUnlink(fsm->path, fsm->mapFlags);
    if (rc == 0)	rc = CPIOERR_ENOENT;
    return (rc ? rc : CPIOERR_ENOENT);	/* XXX HACK */
}

/********************************************************************/

#define	IS_DEV_LOG(_x)	\
	((_x) != NULL && strlen(_x) >= (sizeof("/dev/log")-1) && \
	rstreqn((_x), "/dev/log", sizeof("/dev/log")-1) && \
	((_x)[sizeof("/dev/log")-1] == '\0' || \
	 (_x)[sizeof("/dev/log")-1] == ';'))

/**
 * File state machine driver.
 * @param fsm		file state machine
 * @param stage		next stage
 * @return		0 on success
 */
static int fsmStage(FSM_t fsm, fileStage stage)
{
    static int modulo = 4;
    const char * const cur = fileStageString(stage);
    struct stat * st = &fsm->sb;
    struct stat * ost = &fsm->osb;
    int saveerrno = errno;
    int rc = fsm->rc;
    rpm_loff_t left;

#define	_fafilter(_a)	\
    (!((_a) == FA_CREATE || (_a) == FA_ERASE || (_a) == FA_COPYIN || (_a) == FA_COPYOUT) \
	? fileActionString(_a) : "")

    if (stage & FSM_DEAD) {
	/* do nothing */
    } else if (stage & FSM_INTERNAL) {
	if (_fsm_debug && !(stage & FSM_SYSCALL))
	    rpmlog(RPMLOG_DEBUG, " %8s %06o%3d (%4d,%4d)%10d %s %s\n",
		cur,
		(unsigned)st->st_mode, (int)st->st_nlink,
		(int)st->st_uid, (int)st->st_gid, (int)st->st_size,
		(fsm->path ? fsm->path : ""),
		_fafilter(fsm->action));
    } else {
	fsm->stage = stage;
	if (_fsm_debug || !(stage & FSM_VERBOSE))
	    rpmlog(RPMLOG_DEBUG, "%-8s  %06o%3d (%4d,%4d)%10d %s %s\n",
		cur,
		(unsigned)st->st_mode, (int)st->st_nlink,
		(int)st->st_uid, (int)st->st_gid, (int)st->st_size,
		(fsm->path ? fsm->path : ""),
		_fafilter(fsm->action));
    }
#undef	_fafilter

    switch (stage) {
    case FSM_UNKNOWN:
	break;
    case FSM_PKGINSTALL:
	while (1) {
	    /* Clean fsm, free'ing memory. Read next archive header. */
	    rc = fsmInit(fsm);

	    /* Exit on end-of-payload. */
	    if (rc == CPIOERR_HDR_TRAILER) {
		rc = 0;
		break;
	    }

	    /* Exit on error. */
	    if (rc) {
		fsm->postpone = 1;
		(void) fsmNext(fsm, FSM_UNDO);
		break;
	    }

	    /* Extract file from archive. */
	    rc = fsmNext(fsm, FSM_PROCESS);
	    if (rc) {
		(void) fsmNext(fsm, FSM_UNDO);
		break;
	    }

	    /* Notify on success. */
	    (void) fsmNext(fsm, FSM_NOTIFY);

	    rc = fsmNext(fsm, FSM_FINI);
	    if (rc) {
		break;
	    }
	}
	break;
    case FSM_PKGERASE:
	while (1) {
	    /* Clean fsm, free'ing memory. */
	    rc = fsmInit(fsm);

	    /* Exit on end-of-payload. */
	    if (rc == CPIOERR_HDR_TRAILER) {
		rc = 0;
		break;
	    }

	    /* Rename/erase next item. */
	    if (fsmNext(fsm, FSM_FINI))
		break;

	    /* Notify on success. */
	    (void) fsmNext(fsm, FSM_NOTIFY);
	}
	break;
    case FSM_PKGBUILD:
	while (1) {

	    rc = fsmInit(fsm);

	    /* Exit on end-of-payload. */
	    if (rc == CPIOERR_HDR_TRAILER) {
		rc = 0;
		break;
	    }

	    /* Exit on error. */
	    if (rc) {
		fsm->postpone = 1;
		(void) fsmNext(fsm, FSM_UNDO);
		break;
	    }

	    /* Copy file into archive. */
	    rc = fsmNext(fsm, FSM_PROCESS);
	    if (rc) {
		(void) fsmNext(fsm, FSM_UNDO);
		break;
	    }

	    if (fsmNext(fsm, FSM_FINI))
		break;
	}

	/* Flush partial sets of hard linked files. */
	rc = writeLinks(fsm);

	if (!rc)
	    rc = cpioTrailerWrite(fsm);

	break;
    case FSM_PROCESS:
	if (fsm->postpone) {
	    if (fsm->goal == FSM_PKGINSTALL)
		rc = fsmNext(fsm, FSM_EAT);
	    break;
	}

	if (fsm->goal == FSM_PKGBUILD) {
	    if (fsm->fflags & RPMFILE_GHOST) /* XXX Don't if %ghost file. */
		break;
	    /* Hardlinks are handled later */
	    if (!(S_ISREG(st->st_mode) && st->st_nlink > 1)) {
		rc = writeFile(fsm, 1);
	    }
	    break;
	}

	if (fsm->goal != FSM_PKGINSTALL)
	    break;

	if (S_ISREG(st->st_mode)) {
	    char * path = fsm->path;
	    if (fsm->osuffix)
		fsm->path = fsmFsPath(fsm, st, NULL, NULL);
	    rc = fsmVerify(fsm);

	    if (rc == 0 && fsm->osuffix) {
		char * spath = fsmFsPath(fsm, st, NULL, fsm->osuffix);
		rc = fsmRename(fsm->path, spath, fsm->mapFlags);
		if (!rc)
		    rpmlog(RPMLOG_WARNING, _("%s saved as %s\n"),
			   fsm->path, spath);
		free(spath);
	    }

	    if (fsm->osuffix)
		free(fsm->path);

	    fsm->path = path;
	    if (!(rc == CPIOERR_ENOENT)) return rc;
	    rc = expandRegular(fsm);
	} else if (S_ISDIR(st->st_mode)) {
	    rc = fsmVerify(fsm);
	    if (rc == CPIOERR_ENOENT) {
		mode_t mode = st->st_mode;
		mode &= ~07777;
		mode |=  00700;
		rc = fsmMkdir(fsm->path, mode);
	    }
	} else if (S_ISLNK(st->st_mode)) {
	    if ((st->st_size + 1) > fsm->rdsize) {
		rc = CPIOERR_HDR_SIZE;
		break;
	    }

	    fsm->wrlen = st->st_size;
	    rc = fsmNext(fsm, FSM_DREAD);
	    if (!rc && fsm->rdnb != fsm->wrlen)
		rc = CPIOERR_READ_FAILED;
	    if (rc) break;

	    fsm->wrbuf[st->st_size] = '\0';
	    /* XXX fsmVerify() assumes link target in fsm->wrbuf */
	    rc = fsmVerify(fsm);
	    if (rc == CPIOERR_ENOENT) {
		rc = fsmSymlink(fsm->wrbuf, fsm->path);
	    }
	} else if (S_ISFIFO(st->st_mode)) {
	    /* This mimics cpio S_ISSOCK() behavior but probably isnt' right */
	    rc = fsmVerify(fsm);
	    if (rc == CPIOERR_ENOENT) {
		rc = fsmMkfifo(fsm->path, 0000);
	    }
	} else if (S_ISCHR(st->st_mode) ||
		   S_ISBLK(st->st_mode) ||
    S_ISSOCK(st->st_mode))
	{
	    rc = fsmVerify(fsm);
	    if (rc == CPIOERR_ENOENT) {
		rc = fsmMknod(fsm->path, fsm->sb.st_mode, fsm->sb.st_rdev);
	    }
	} else {
	    /* XXX Special case /dev/log, which shouldn't be packaged anyways */
	    if (!IS_DEV_LOG(fsm->path))
		rc = CPIOERR_UNKNOWN_FILETYPE;
	}
	if (S_ISREG(st->st_mode) && st->st_nlink > 1) {
	    fsm->li->createdPath = fsm->li->linkIndex;
	    rc = fsmMakeLinks(fsm);
	}
	break;
    case FSM_POST:
	break;
    case FSM_NOTIFY:		/* XXX move from fsm to psm -> tsm */
	if (fsm->goal == FSM_PKGINSTALL) {
	    rpmpsmNotify(fsm->psm, RPMCALLBACK_INST_PROGRESS, fsm->cpioPos);
	} else if (fsm->goal == FSM_PKGERASE) {
	    /* On erase we're iterating backwards, fixup for progress */
	    rpm_loff_t amount = (fsm->ix >= 0) ?
				rpmfiFC(fsmGetFi(fsm)) - fsm->ix : 0;
	    rpmpsmNotify(fsm->psm, RPMCALLBACK_UNINST_PROGRESS, amount);
	}
	break;
    case FSM_UNDO:
	if (fsm->postpone)
	    break;
	if (fsm->goal == FSM_PKGINSTALL) {
	    /* XXX only erase if temp fn w suffix is in use */
	    if (fsm->suffix) {
		if (S_ISDIR(st->st_mode)) {
		    (void) fsmRmdir(fsm->path);
		} else {
		    (void) fsmUnlink(fsm->path, fsm->mapFlags);
		}
	    }
	    errno = saveerrno;
	}
	if (fsm->failedFile && *fsm->failedFile == NULL)
	    *fsm->failedFile = xstrdup(fsm->path);
	break;
    case FSM_FINI:
	if (!fsm->postpone) {
	    if (fsm->goal == FSM_PKGINSTALL)
		rc = ((S_ISREG(st->st_mode) && st->st_nlink > 1)
			? fsmCommitLinks(fsm) : fsmNext(fsm, FSM_COMMIT));
	    if (fsm->goal == FSM_PKGERASE)
		rc = fsmNext(fsm, FSM_COMMIT);
	}
	fsm->path = _free(fsm->path);
	memset(st, 0, sizeof(*st));
	memset(ost, 0, sizeof(*ost));
	break;
    case FSM_COMMIT:
	/* Rename pre-existing modified or unmanaged file. */
	if (fsm->osuffix && fsm->diskchecked &&
	  (fsm->exists || (fsm->goal == FSM_PKGINSTALL && S_ISREG(st->st_mode))))
	{
	    char * opath = fsmFsPath(fsm, st, NULL, NULL);
	    char * path = fsmFsPath(fsm, st, NULL, fsm->osuffix);
	    rc = fsmRename(opath, path, fsm->mapFlags);
	    if (!rc) {
		rpmlog(RPMLOG_WARNING, _("%s saved as %s\n"), opath, path);
	    }
	    free(path);
	    free(opath);
	}

	/* Remove erased files. */
	if (fsm->goal == FSM_PKGERASE) {
	    if (fsm->action == FA_ERASE) {
		rpmte te = fsmGetTe(fsm);
		if (S_ISDIR(st->st_mode)) {
		    rc = fsmRmdir(fsm->path);
		    if (!rc) break;
		    switch (rc) {
		    case CPIOERR_ENOENT: /* XXX rmdir("/") linux 2.2.x kernel hack */
		    case CPIOERR_ENOTEMPTY:
	/* XXX make sure that build side permits %missingok on directories. */
			if (fsm->fflags & RPMFILE_MISSINGOK)
			    break;

			/* XXX common error message. */
			rpmlog(
			    (strict_erasures ? RPMLOG_ERR : RPMLOG_DEBUG),
			    _("%s rmdir of %s failed: Directory not empty\n"), 
				rpmteTypeString(te), fsm->path);
			break;
		    default:
			rpmlog(
			    (strict_erasures ? RPMLOG_ERR : RPMLOG_DEBUG),
				_("%s rmdir of %s failed: %s\n"),
				rpmteTypeString(te), fsm->path, strerror(errno));
			break;
		    }
		} else {
		    rc = fsmUnlink(fsm->path, fsm->mapFlags);
		    if (!rc) break;
		    switch (rc) {
		    case CPIOERR_ENOENT:
			if (fsm->fflags & RPMFILE_MISSINGOK)
			    break;
		    default:
			rpmlog(
			    (strict_erasures ? RPMLOG_ERR : RPMLOG_DEBUG),
				_("%s unlink of %s failed: %s\n"),
				rpmteTypeString(te), fsm->path, strerror(errno));
			break;
		    }
		}
	    }
	    /* XXX Failure to remove is not (yet) cause for failure. */
	    if (!strict_erasures) rc = 0;
	    break;
	}

	/* XXX Special case /dev/log, which shouldn't be packaged anyways */
	if (!S_ISSOCK(st->st_mode) && !IS_DEV_LOG(fsm->path)) {
	    /* Rename temporary to final file name. */
	    if (!S_ISDIR(st->st_mode) && (fsm->suffix || fsm->nsuffix)) {
		char *npath = fsmFsPath(fsm, st, NULL, fsm->nsuffix);
		rc = fsmRename(fsm->path, npath, fsm->mapFlags);
		if (!rc && fsm->nsuffix) {
		    char * opath = fsmFsPath(fsm, st, NULL, NULL);
		    rpmlog(RPMLOG_WARNING, _("%s created as %s\n"),
			   opath, npath);
		    free(opath);
		}
		free(fsm->path);
		fsm->path = npath;
	    }
	    /* Set file security context (if enabled) */
	    if (!rc && !getuid()) {
		rc = fsmSetSELabel(fsm->sehandle, fsm->path, fsm->sb.st_mode);
	    }
	    if (S_ISLNK(st->st_mode)) {
		if (!rc && !getuid())
		    rc = fsmLChown(fsm->path, fsm->sb.st_uid, fsm->sb.st_gid);
	    } else {
		rpmfi fi = fsmGetFi(fsm);
		if (!rc && !getuid())
		    rc = fsmChown(fsm->path, fsm->sb.st_uid, fsm->sb.st_gid);
		if (!rc)
		    rc = fsmChmod(fsm->path, fsm->sb.st_mode);
		if (!rc) {
		    rc = fsmUtime(fsm->path, rpmfiFMtimeIndex(fi, fsm->ix));
		    /* utime error is not critical for directories */
		    if (rc && S_ISDIR(st->st_mode))
			rc = 0;
		}
		/* Set file capabilities (if enabled) */
		if (!rc && !S_ISDIR(st->st_mode) && !getuid()) {
		    rc = fsmSetFCaps(fsm->path, rpmfiFCapsIndex(fi, fsm->ix));
		}
	    }
	}

	/* Notify on success. */
	if (!rc)		rc = fsmNext(fsm, FSM_NOTIFY);
	else if (fsm->failedFile && *fsm->failedFile == NULL) {
	    *fsm->failedFile = fsm->path;
	    fsm->path = NULL;
	}
	break;
    case FSM_DESTROY:
	fsm->path = _free(fsm->path);

	/* Check for hard links missing from payload. */
	while ((fsm->li = fsm->links) != NULL) {
	    fsm->links = fsm->li->next;
	    fsm->li->next = NULL;
	    if (fsm->goal == FSM_PKGINSTALL && fsm->li->linksLeft) {
		for (nlink_t i = 0 ; i < fsm->li->linksLeft; i++) {
		    if (fsm->li->filex[i] < 0)
			continue;
		    rc = CPIOERR_MISSING_HARDLINK;
		    if (fsm->failedFile && *fsm->failedFile == NULL) {
			fsm->ix = fsm->li->filex[i];
			if (!fsmMapPath(fsm)) {
	    		    /* Out-of-sync hardlinks handled as sub-state */
			    *fsm->failedFile = fsm->path;
			    fsm->path = NULL;
			}
		    }
		    break;
		}
	    }
	    fsm->li = freeHardLink(fsm->li);
	}
	fsm->rdbuf = fsm->rdb = _free(fsm->rdb);
	fsm->wrbuf = fsm->wrb = _free(fsm->wrb);
	break;
    case FSM_NEXT:
	rc = fsmNext(fsm, FSM_POS);
	if (!rc)
	    rc = cpioHeaderRead(fsm, st);	/* Read next payload header. */
	if (rc) break;
	if (rstreq(fsm->path, CPIO_TRAILER)) { /* Detect end-of-payload. */
	    fsm->path = _free(fsm->path);
	    rc = CPIOERR_HDR_TRAILER;
	}
	if (!rc)
	    rc = fsmNext(fsm, FSM_POS);
	break;
    case FSM_EAT:
	for (left = st->st_size; left > 0; left -= fsm->rdnb) {
	    fsm->wrlen = (left > fsm->wrsize ? fsm->wrsize : left);
	    rc = fsmNext(fsm, FSM_DREAD);
	    if (rc)
		break;
	}
	break;
    case FSM_POS:
	left = (modulo - (fsm->cpioPos % modulo)) % modulo;
	if (left) {
	    fsm->wrlen = left;
	    (void) fsmNext(fsm, FSM_DREAD);
	}
	break;
    case FSM_PAD:
	left = (modulo - (fsm->cpioPos % modulo)) % modulo;
	if (left) {
	    memset(fsm->rdbuf, 0, left);
	    /* XXX DWRITE uses rdnb for I/O length. */
	    fsm->rdnb = left;
	    (void) fsmNext(fsm, FSM_DWRITE);
	}
	break;
    case FSM_DREAD:
	fsm->rdnb = Fread(fsm->wrbuf, sizeof(*fsm->wrbuf), fsm->wrlen, fsm->cfd);
	if (_fsm_debug && (stage & FSM_SYSCALL))
	    rpmlog(RPMLOG_DEBUG, " %8s (%s, %d, cfd)\trdnb %d\n",
		cur, (fsm->wrbuf == fsm->wrb ? "wrbuf" : "mmap"),
		(int)fsm->wrlen, (int)fsm->rdnb);
	if (fsm->rdnb != fsm->wrlen || Ferror(fsm->cfd))
	    rc = CPIOERR_READ_FAILED;
	if (fsm->rdnb > 0)
	    fsm->cpioPos += fsm->rdnb;
	break;
    case FSM_DWRITE:
	fsm->wrnb = Fwrite(fsm->rdbuf, sizeof(*fsm->rdbuf), fsm->rdnb, fsm->cfd);
	if (_fsm_debug && (stage & FSM_SYSCALL))
	    rpmlog(RPMLOG_DEBUG, " %8s (%s, %d, cfd)\twrnb %d\n",
		cur, (fsm->rdbuf == fsm->rdb ? "rdbuf" : "mmap"),
		(int)fsm->rdnb, (int)fsm->wrnb);
	if (fsm->rdnb != fsm->wrnb || Ferror(fsm->cfd))
	    rc = CPIOERR_WRITE_FAILED;
	if (fsm->wrnb > 0)
	    fsm->cpioPos += fsm->wrnb;
	break;

    default:
	break;
    }

    if (!(stage & FSM_INTERNAL)) {
	fsm->rc = (rc == CPIOERR_HDR_TRAILER ? 0 : rc);
    }
    return rc;
}

/**
 * Return formatted string representation of file disposition.
 * @param a		file dispostion
 * @return		formatted string
 */
static const char * fileActionString(rpmFileAction a)
{
    switch (a) {
    case FA_UNKNOWN:	return "unknown";
    case FA_CREATE:	return "create";
    case FA_COPYOUT:	return "copyout";
    case FA_COPYIN:	return "copyin";
    case FA_BACKUP:	return "backup";
    case FA_SAVE:	return "save";
    case FA_SKIP:	return "skip";
    case FA_ALTNAME:	return "altname";
    case FA_ERASE:	return "erase";
    case FA_SKIPNSTATE: return "skipnstate";
    case FA_SKIPNETSHARED: return "skipnetshared";
    case FA_SKIPCOLOR:	return "skipcolor";
    default:		return "???";
    }
}

/**
 * Return formatted string representation of file stages.
 * @param a		file stage
 * @return		formatted string
 */
static const char * fileStageString(fileStage a)
{
    switch(a) {
    case FSM_UNKNOWN:	return "unknown";

    case FSM_PKGINSTALL:return "INSTALL";
    case FSM_PKGERASE:	return "ERASE";
    case FSM_PKGBUILD:	return "BUILD";
    case FSM_PKGUNDO:	return "UNDO";

    case FSM_CREATE:	return "create";
    case FSM_INIT:	return "init";
    case FSM_MAP:	return "map";
    case FSM_MKDIRS:	return "mkdirs";
    case FSM_RMDIRS:	return "rmdirs";
    case FSM_PRE:	return "pre";
    case FSM_PROCESS:	return "process";
    case FSM_POST:	return "post";
    case FSM_MKLINKS:	return "mklinks";
    case FSM_NOTIFY:	return "notify";
    case FSM_UNDO:	return "undo";
    case FSM_FINI:	return "fini";
    case FSM_COMMIT:	return "commit";
    case FSM_DESTROY:	return "destroy";
    case FSM_VERIFY:	return "verify";

    case FSM_UNLINK:	return "unlink";
    case FSM_RENAME:	return "rename";
    case FSM_MKDIR:	return "mkdir";
    case FSM_RMDIR:	return "rmdir";
    case FSM_LSETFCON:	return "lsetfcon";
    case FSM_CHOWN:	return "chown";
    case FSM_LCHOWN:	return "lchown";
    case FSM_CHMOD:	return "chmod";
    case FSM_UTIME:	return "utime";
    case FSM_SYMLINK:	return "symlink";
    case FSM_LINK:	return "link";
    case FSM_MKFIFO:	return "mkfifo";
    case FSM_MKNOD:	return "mknod";
    case FSM_LSTAT:	return "lstat";
    case FSM_STAT:	return "stat";
    case FSM_READLINK:	return "readlink";
    case FSM_SETCAP:	return "setcap";

    case FSM_NEXT:	return "next";
    case FSM_EAT:	return "eat";
    case FSM_POS:	return "pos";
    case FSM_PAD:	return "pad";
    case FSM_TRAILER:	return "trailer";
    case FSM_HREAD:	return "hread";
    case FSM_HWRITE:	return "hwrite";
    case FSM_DREAD:	return "Fread";
    case FSM_DWRITE:	return "Fwrite";

    default:		return "???";
    }
}

int rpmfsmRun(fileStage goal, rpmts ts, rpmte te, rpmfi fi, FD_t cfd,
	      rpmpsm psm, rpm_loff_t * archiveSize, char ** failedFile)
{
    struct fsm_s fsm;
    int sc = 0;
    int ec = 0;

    memset(&fsm, 0, sizeof(fsm));
    sc = fsmSetup(&fsm, goal, ts, te, fi, cfd, psm, archiveSize, failedFile);
    ec = fsmTeardown(&fsm);

    /* Return the relevant code: if setup failed, teardown doesn't matter */
    return (sc ? sc : ec);
}
