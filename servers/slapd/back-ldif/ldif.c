/* ldif.c - the ldif backend */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2005-2008 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was originally developed by Eric Stokes for inclusion
 * in OpenLDAP Software.
 */

#include "portable.h"
#include <stdio.h>
#include <ac/string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ac/dirent.h>
#include <fcntl.h>
#include <ac/errno.h>
#include <ac/unistd.h>
#include "slap.h"
#include "lutil.h"
#include "config.h"

typedef struct enumCookie {
	Operation *op;
	SlapReply *rs;
	Entry **entries;
	ID elen;
	ID eind;
} enumCookie;

struct ldif_info {
	struct berval li_base_path;
	enumCookie li_tool_cookie;
	ID li_tool_current;
	ldap_pvt_thread_rdwr_t  li_rdwr;
};

#ifdef _WIN32
#define mkdir(a,b)	mkdir(a)
#define move_file(from, to) (!MoveFileEx(from, to, MOVEFILE_REPLACE_EXISTING))
#else
#define move_file(from, to) rename(from, to)
#endif
#define move_dir(from, to) rename(from, to)


#define LDIF	".ldif"
#define LDIF_FILETYPE_SEP	'.'			/* LDIF[0] */

/*
 * Unsafe/translated characters in the filesystem.
 *
 * LDIF_UNSAFE_CHAR(c) returns true if the character c is not to be used
 * in relative filenames, except it should accept '\\', '{' and '}' even
 * if unsafe.  The value should be a constant expression.
 *
 * If '\\' is unsafe, #define LDIF_ESCAPE_CHAR as a safe character.
 * If '{' and '}' are unsafe, #define IX_FSL/IX_FSR as safe characters.
 * (Not digits, '-' or '+'.  IX_FSL == IX_FSR is allowed.)
 *
 * Characters are escaped as LDIF_ESCAPE_CHAR followed by two hex digits,
 * except '\\' is replaced with LDIF_ESCAPE_CHAR and {} with IX_FS[LR].
 * Also some LDIF special chars are hex-escaped.
 *
 * Thus an LDIF filename is a valid normalized RDN (or suffix DN)
 * followed by ".ldif", except with '\\' replaced with LDIF_ESCAPE_CHAR.
 */

#ifndef _WIN32

/*
 * Unix/MacOSX version.  ':' vs '/' can cause confusion on MacOSX so we
 * escape both.  We escape them on Unix so both OS variants get the same
 * filenames.
 */
#define LDIF_ESCAPE_CHAR	'\\'
#define LDIF_UNSAFE_CHAR(c)	((c) == '/' || (c) == ':')

#else /* _WIN32 */

/* Windows version - Microsoft's list of unsafe characters, except '\\' */
#define LDIF_ESCAPE_CHAR	'^'			/* Not '\\' (unsafe on Windows) */
#define LDIF_UNSAFE_CHAR(c)	\
	((c) == '/' || (c) == ':' || \
	 (c) == '<' || (c) == '>' || (c) == '"' || \
	 (c) == '|' || (c) == '?' || (c) == '*')

#endif /* !_WIN32 */

/*
 * Left and Right "{num}" prefix to ordered RDNs ("olcDatabase={1}bdb").
 * IX_DN* are for LDAP RDNs, IX_FS* for their .ldif filenames.
 */
#define IX_DNL	'{'
#define	IX_DNR	'}'
#ifndef IX_FSL
#define	IX_FSL	IX_DNL
#define IX_FSR	IX_DNR
#endif

/*
 * Test for unsafe chars, as well as chars handled specially by back-ldif:
 * - If the escape char is not '\\', it must itself be escaped.  Otherwise
 *   '\\' and the escape char would map to the same character.
 * - Escape the '.' in ".ldif", so the directory for an RDN that actually
 *   ends with ".ldif" can not conflict with a file of the same name.  And
 *   since some OSes/programs choke on multiple '.'s, escape all of them.
 * - If '{' and '}' are translated to some other characters, those
 *   characters must in turn be escaped when they occur in an RDN.
 */
#ifndef LDIF_NEED_ESCAPE
#define	LDIF_NEED_ESCAPE(c) \
	((LDIF_UNSAFE_CHAR(c)) || \
	 LDIF_MAYBE_UNSAFE(c, LDIF_ESCAPE_CHAR) || \
	 LDIF_MAYBE_UNSAFE(c, LDIF_FILETYPE_SEP) || \
	 LDIF_MAYBE_UNSAFE(c, IX_FSL) || \
	 (IX_FSR != IX_FSL && LDIF_MAYBE_UNSAFE(c, IX_FSR)))
#endif
/*
 * Helper macro for LDIF_NEED_ESCAPE(): Treat character x as unsafe if
 * back-ldif does not already treat is specially.
 */
#define LDIF_MAYBE_UNSAFE(c, x) \
	(!(LDIF_UNSAFE_CHAR(x) || (x) == '\\' || (x) == IX_DNL || (x) == IX_DNR) \
	 && (c) == (x))

/* Collect other "safe char" tests here, until someone needs a fix. */
enum {
	safe_filenames = STRLENOF("" LDAP_DIRSEP "") == 1 && !(
		LDIF_UNSAFE_CHAR('-') || /* for "{-1}frontend" in bconfig.c */
		LDIF_UNSAFE_CHAR(LDIF_ESCAPE_CHAR) ||
		LDIF_UNSAFE_CHAR(IX_FSL) || LDIF_UNSAFE_CHAR(IX_FSR))
};
/* Sanity check: Try to force a compilation error if !safe_filenames */
typedef struct {
	int assert_safe_filenames : safe_filenames ? 2 : -2;
} assert_safe_filenames[safe_filenames ? 2 : -2];


#define ENTRY_BUFF_INCREMENT 500

static ConfigTable ldifcfg[] = {
	{ "directory", "dir", 2, 2, 0, ARG_BERVAL|ARG_OFFSET,
		(void *)offsetof(struct ldif_info, li_base_path),
		"( OLcfgDbAt:0.1 NAME 'olcDbDirectory' "
			"DESC 'Directory for database content' "
			"EQUALITY caseIgnoreMatch "
			"SYNTAX OMsDirectoryString SINGLE-VALUE )", NULL, NULL },
	{ NULL, NULL, 0, 0, 0, ARG_IGNORED,
		NULL, NULL, NULL, NULL }
};

static ConfigOCs ldifocs[] = {
	{ "( OLcfgDbOc:2.1 "
		"NAME 'olcLdifConfig' "
		"DESC 'LDIF backend configuration' "
		"SUP olcDatabaseConfig "
		"MUST ( olcDbDirectory ) )", Cft_Database, ldifcfg },
	{ NULL, 0, NULL }
};


/*
 * Handle file/directory names.
 */

/* Set *res = LDIF filename path for the normalized DN */
static void
dn2path( BackendDB *be, struct berval *dn, struct berval *res )
{
	struct ldif_info *li = (struct ldif_info *) be->be_private;
	struct berval *suffixdn = &be->be_nsuffix[0];
	const char *start, *end, *next, *p;
	char ch, *ptr;
	ber_len_t len;
	static const char hex[] = "0123456789ABCDEF";

	assert( dn != NULL );
	assert( !BER_BVISNULL( dn ) );
	assert( suffixdn != NULL );
	assert( !BER_BVISNULL( suffixdn ) );
	assert( dnIsSuffix( dn, suffixdn ) );

	start = dn->bv_val;
	end = start + dn->bv_len;

	/* Room for dir, dirsep, dn, LDIF, "\hexpair"-escaping of unsafe chars */
	len = li->li_base_path.bv_len + dn->bv_len + (1 + STRLENOF( LDIF ));
	for ( p = start; p < end; ) {
		ch = *p++;
		if ( LDIF_NEED_ESCAPE( ch ) )
			len += 2;
	}
	res->bv_val = ch_malloc( len + 1 );

	ptr = lutil_strcopy( res->bv_val, li->li_base_path.bv_val );
	for ( next = end - suffixdn->bv_len; end > start; end = next ) {
		/* Set p = start of DN component, next = &',' or start of DN */
		while ( (p = next) > start ) {
			--next;
			if ( DN_SEPARATOR( *next ) )
				break;
		}
		/* Append <dirsep> <p..end-1: RDN or database-suffix> */
		for ( *ptr++ = LDAP_DIRSEP[0]; p < end; *ptr++ = ch ) {
			ch = *p++;
			if ( LDIF_ESCAPE_CHAR != '\\' && ch == '\\' ) {
				ch = LDIF_ESCAPE_CHAR;
			} else if ( IX_FSL != IX_DNL && ch == IX_DNL ) {
				ch = IX_FSL;
			} else if ( IX_FSR != IX_DNR && ch == IX_DNR ) {
				ch = IX_FSR;
			} else if ( LDIF_NEED_ESCAPE( ch ) ) {
				*ptr++ = LDIF_ESCAPE_CHAR;
				*ptr++ = hex[(ch & 0xFFU) >> 4];
				ch = hex[ch & 0x0FU];
			}
		}
	}
	ptr = lutil_strcopy( ptr, LDIF );
	res->bv_len = ptr - res->bv_val;

	assert( res->bv_len <= len );
}

/* .ldif entry filename length <-> subtree dirname length. */
#define ldif2dir_len(bv)  ((bv).bv_len -= STRLENOF(LDIF))
#define dir2ldif_len(bv)  ((bv).bv_len += STRLENOF(LDIF))
/* .ldif entry filename <-> subtree dirname, both with dirname length. */
#define ldif2dir_name(bv) ((bv).bv_val[(bv).bv_len] = '\0')
#define dir2ldif_name(bv) ((bv).bv_val[(bv).bv_len] = LDIF_FILETYPE_SEP)

/* Get the parent directory path, plus the LDIF suffix overwritten by a \0. */
static int
get_parent_path( struct berval *dnpath, struct berval *res )
{
	ber_len_t i = dnpath->bv_len;

	while ( i > 0 && dnpath->bv_val[ --i ] != LDAP_DIRSEP[0] ) ;
	if ( res == NULL ) {
		res = dnpath;
	} else {
		res->bv_val = SLAP_MALLOC( i + 1 + STRLENOF(LDIF) );
		if ( res->bv_val == NULL )
			return LDAP_OTHER;
		AC_MEMCPY( res->bv_val, dnpath->bv_val, i );
	}
	res->bv_len = i;
	strcpy( res->bv_val + i, LDIF );
	res->bv_val[i] = '\0';
	return LDAP_SUCCESS;
}

/* Make temporary filename pattern for mkstemp() based on dnpath. */
static char *
ldif_tempname( const struct berval *dnpath )
{
	static const char suffix[] = ".XXXXXX";
	ber_len_t len = dnpath->bv_len - STRLENOF( LDIF );
	char *name = SLAP_MALLOC( len + sizeof( suffix ) );

	if ( name != NULL ) {
		AC_MEMCPY( name, dnpath->bv_val, len );
		strcpy( name + len, suffix );
	}
	return name;
}

/*
 * Read a file, or stat() it if datap == NULL.  Allocate and fill *datap.
 * Return LDAP_SUCCESS, LDAP_NO_SUCH_OBJECT (no such file), or another error.
 */
static int
ldif_read_file( const char *path, char **datap )
{
	int rc, fd, len;
	int res = -1;	/* 0:success, <0:error, >0:file too big/growing. */
	struct stat st;
	char *data = NULL, *ptr;

	if ( datap == NULL ) {
		res = stat( path, &st );
		goto done;
	}
	fd = open( path, O_RDONLY );
	if ( fd >= 0 ) {
		if ( fstat( fd, &st ) == 0 ) {
			if ( st.st_size > INT_MAX - 2 ) {
				res = 1;
			} else {
				len = st.st_size + 1; /* +1 detects file size > st.st_size */
				*datap = data = ptr = SLAP_MALLOC( len + 1 );
				if ( ptr != NULL ) {
					while ( len && (res = read( fd, ptr, len )) ) {
						if ( res > 0 ) {
							len -= res;
							ptr += res;
						} else if ( errno != EINTR ) {
							break;
						}
					}
					*ptr = '\0';
				}
			}
		}
		if ( close( fd ) < 0 )
			res = -1;
	}

 done:
	if ( res == 0 ) {
		Debug( LDAP_DEBUG_TRACE, "ldif_read_file: %s: \"%s\"\n",
			datap ? "read entry file" : "entry file exists", path, 0 );
		rc = LDAP_SUCCESS;
	} else {
		if ( res < 0 && errno == ENOENT ) {
			Debug( LDAP_DEBUG_TRACE, "ldif_read_file: "
				"no entry file \"%s\"\n", path, 0, 0 );
			rc = LDAP_NO_SUCH_OBJECT;
		} else {
			const char *msg = res < 0 ? STRERROR( errno ) : "bad stat() size";
			Debug( LDAP_DEBUG_ANY, "ldif_read_file: %s for \"%s\"\n",
				msg, path, 0 );
			rc = LDAP_OTHER;
		}
		if ( data != NULL )
			SLAP_FREE( data );
	}
	return rc;
}

/*
 * return nonnegative for success or -1 for error
 * do not return numbers less than -1
 */
static int
spew_file( int fd, const char *spew, int len, int *save_errno )
{
	int writeres = 0;

	while(len > 0) {
		writeres = write(fd, spew, len);
		if(writeres == -1) {
			*save_errno = errno;
			if (*save_errno != EINTR)
				break;
		}
		else {
			spew += writeres;
			len -= writeres;
		}
	}
	return writeres;
}

/* Write an entry LDIF file.  Create parentdir first if non-NULL. */
static int
ldif_write_entry(
	Operation *op,
	Entry *e,
	const struct berval *path,
	const char *parentdir,
	const char **text )
{
	int rc = LDAP_OTHER, res, save_errno = 0;
	int fd, entry_length;
	char *entry_as_string, *tmpfname;

	if ( parentdir != NULL && mkdir( parentdir, 0750 ) < 0 ) {
		save_errno = errno;
		Debug( LDAP_DEBUG_ANY, "ldif_write_entry: %s \"%s\": %s\n",
			"cannot create parent directory",
			parentdir, STRERROR( save_errno ) );
		*text = "internal error (cannot create parent directory)";
		return rc;
	}

	tmpfname = ldif_tempname( path );
	fd = tmpfname == NULL ? -1 : mkstemp( tmpfname );
	if ( fd < 0 ) {
		save_errno = errno;
		Debug( LDAP_DEBUG_ANY, "ldif_write_entry: %s for \"%s\": %s\n",
			"cannot create file", e->e_dn, STRERROR( save_errno ) );
		*text = "internal error (cannot create file)";

	} else {
		ber_len_t dn_len = e->e_name.bv_len;
		struct berval rdn;

		/* Only save the RDN onto disk */
		dnRdn( &e->e_name, &rdn );
		if ( rdn.bv_len != dn_len ) {
			e->e_name.bv_val[rdn.bv_len] = '\0';
			e->e_name.bv_len = rdn.bv_len;
		}

		res = -2;
		ldap_pvt_thread_mutex_lock( &entry2str_mutex );
		entry_as_string = entry2str( e, &entry_length );
		if ( entry_as_string != NULL )
			res = spew_file( fd, entry_as_string, entry_length, &save_errno );
		ldap_pvt_thread_mutex_unlock( &entry2str_mutex );

		/* Restore full DN */
		if ( rdn.bv_len != dn_len ) {
			e->e_name.bv_val[rdn.bv_len] = ',';
			e->e_name.bv_len = dn_len;
		}

		if ( close( fd ) < 0 && res >= 0 ) {
			res = -1;
			save_errno = errno;
		}

		if ( res >= 0 ) {
			if ( move_file( tmpfname, path->bv_val ) == 0 ) {
				Debug( LDAP_DEBUG_TRACE, "ldif_write_entry: "
					"wrote entry \"%s\"\n", e->e_name.bv_val, 0, 0 );
				rc = LDAP_SUCCESS;
			} else {
				save_errno = errno;
				Debug( LDAP_DEBUG_ANY, "ldif_write_entry: "
					"could not put entry file for \"%s\" in place: %s\n",
					e->e_name.bv_val, STRERROR( save_errno ), 0 );
				*text = "internal error (could not put entry file in place)";
			}
		} else if ( res == -1 ) {
			Debug( LDAP_DEBUG_ANY, "ldif_write_entry: %s \"%s\": %s\n",
				"write error to", tmpfname, STRERROR( save_errno ) );
			*text = "internal error (write error to entry file)";
		}

		if ( rc != LDAP_SUCCESS ) {
			unlink( tmpfname );
		}
	}

	if ( tmpfname )
		SLAP_FREE( tmpfname );
	return rc;
}

/*
 * Read the entry at path, or if entryp==NULL just see if it exists.
 * pdn and pndn are the parent's DN and normalized DN, or both NULL.
 * Return an LDAP result code.
 */
static int
ldif_read_entry(
	Operation *op,
	const char *path,
	struct berval *pdn,
	struct berval *pndn,
	Entry **entryp,
	const char **text )
{
	int rc;
	Entry *entry;
	char *entry_as_string;
	struct berval rdn;

	rc = ldif_read_file( path, entryp ? &entry_as_string : NULL );

	switch ( rc ) {
	case LDAP_SUCCESS:
		if ( entryp == NULL )
			break;
		*entryp = entry = str2entry( entry_as_string );
		SLAP_FREE( entry_as_string );
		if ( entry == NULL ) {
			rc = LDAP_OTHER;
			if ( text != NULL )
				*text = "internal error (cannot parse some entry file)";
			break;
		}
		if ( pdn == NULL || BER_BVISEMPTY( pdn ) )
			break;
		/* Append parent DN to DN from LDIF file */
		rdn = entry->e_name;
		build_new_dn( &entry->e_name, pdn, &rdn, NULL );
		SLAP_FREE( rdn.bv_val );
		rdn = entry->e_nname;
		build_new_dn( &entry->e_nname, pndn, &rdn, NULL );
		SLAP_FREE( rdn.bv_val );
		break;

	case LDAP_OTHER:
		if ( text != NULL )
			*text = entryp
				? "internal error (cannot read some entry file)"
				: "internal error (cannot stat some entry file)";
		break;
	}

	return rc;
}

/*
 * Read the operation's entry, or if entryp==NULL just see if it exists.
 * Return an LDAP result code.  May set *text to a message on failure.
 * If pathp is non-NULL, set it to the entry filename on success.
 */
static int
get_entry(
	Operation *op,
	Entry **entryp,
	struct berval *pathp,
	const char **text )
{
	int rc;
	struct berval path, pdn, pndn;

	dnParent(&op->o_req_dn, &pdn);
	dnParent(&op->o_req_ndn, &pndn);
	dn2path( op->o_bd, &op->o_req_ndn, &path );
	rc = ldif_read_entry( op, path.bv_val, &pdn, &pndn, entryp, text );

	if ( rc == LDAP_SUCCESS && pathp != NULL ) {
		*pathp = path;
	} else {
		SLAP_FREE(path.bv_val);
	}
	return rc;
}

static void fullpath(struct berval *base, struct berval *name, struct berval *res) {
	char *ptr;
	res->bv_len = name->bv_len + base->bv_len + 1;
	res->bv_val = ch_malloc( res->bv_len + 1 );
	strcpy(res->bv_val, base->bv_val);
	ptr = res->bv_val + base->bv_len;
	*ptr++ = LDAP_DIRSEP[0];
	strcpy(ptr, name->bv_val);
}

typedef struct bvlist {
	struct bvlist *next;
	struct berval bv;
	struct berval num;
	int inum;
	int off;
} bvlist;


static int r_enum_tree(enumCookie *ck, struct berval *path, int base,
	struct berval *pdn, struct berval *pndn)
{
	Entry *e = NULL;
	int fd = 0, rc = LDAP_SUCCESS;

	if ( !base ) {
		rc = ldif_read_entry( ck->op, path->bv_val, pdn, pndn, &e,
			ck->rs == NULL ? NULL : &ck->rs->sr_text );
		if ( rc != LDAP_SUCCESS ) {
			return LDAP_NO_SUCH_OBJECT;
		}

		if ( ck->op->ors_scope == LDAP_SCOPE_BASE ||
			ck->op->ors_scope == LDAP_SCOPE_SUBTREE ) {
			/* Send right away? */
			if ( ck->rs ) {
				/*
				 * If it's a referral entry, send a continuation reference.
				 * (ldif_back_referrals() handles baseobject referrals.)
				 * Don't check the filter since it's only a candidate.
				 */
				if ( !get_manageDSAit( ck->op )
						&& is_entry_referral( e ) )
				{
					BerVarray erefs = get_entry_referrals( ck->op, e );
					ck->rs->sr_ref = referral_rewrite( erefs,
							&e->e_name, NULL,
							ck->op->ors_scope );
	
					ck->rs->sr_entry = e;
					rc = send_search_reference( ck->op, ck->rs );
					ber_bvarray_free( ck->rs->sr_ref );
					ber_bvarray_free( erefs );
					ck->rs->sr_ref = NULL;
					ck->rs->sr_entry = NULL;
	
				} else if ( test_filter( ck->op, e, ck->op->ors_filter ) == LDAP_COMPARE_TRUE )
				{
					ck->rs->sr_entry = e;
					ck->rs->sr_attrs = ck->op->ors_attrs;
					ck->rs->sr_flags = REP_ENTRY_MODIFIABLE;
					rc = send_search_entry(ck->op, ck->rs);
					ck->rs->sr_entry = NULL;
				}
				fd = 1;
				if ( rc )
					goto done;
			} else {
			/* Queueing up for tool mode */
				if(ck->entries == NULL) {
					ck->entries = (Entry **) ch_malloc(sizeof(Entry *) * ENTRY_BUFF_INCREMENT);
					ck->elen = ENTRY_BUFF_INCREMENT;
				}
				if(ck->eind >= ck->elen) { /* grow entries if necessary */	
					ck->entries = (Entry **) ch_realloc(ck->entries, sizeof(Entry *) * (ck->elen) * 2);
					ck->elen *= 2;
				}
	
				ck->entries[ck->eind++] = e;
				fd = 0;
			}
		} else {
			fd = 1;
		}
	}

	if ( ck->op->ors_scope != LDAP_SCOPE_BASE ) {
		DIR * dir_of_path;
		bvlist *list = NULL, *ptr;

		path->bv_len -= STRLENOF( LDIF );
		path->bv_val[path->bv_len] = '\0';

		dir_of_path = opendir(path->bv_val);
		if(dir_of_path == NULL) { /* can't open directory */
			if ( errno != ENOENT ) {
				/* it shouldn't be treated as an error
				 * only if the directory doesn't exist */
				rc = LDAP_BUSY;
				Debug( LDAP_DEBUG_ANY,
					"=> ldif_enum_tree: failed to opendir %s (%d)\n",
					path->bv_val, errno, 0 );
			}
			goto done;
		}
	
		while(1) {
			struct berval fname, itmp;
			struct dirent * dir;
			bvlist *bvl, **prev;

			dir = readdir(dir_of_path);
			if(dir == NULL) break; /* end of the directory */
			fname.bv_len = strlen( dir->d_name );
			if ( fname.bv_len <= STRLENOF( LDIF ))
				continue;
			if ( strcmp( dir->d_name + (fname.bv_len - STRLENOF(LDIF)), LDIF))
				continue;
			fname.bv_val = dir->d_name;

			bvl = ch_malloc( sizeof(bvlist) );
			ber_dupbv( &bvl->bv, &fname );
			BER_BVZERO( &bvl->num );
			itmp.bv_val = ber_bvchr( &bvl->bv, IX_FSL );
			if ( itmp.bv_val ) {
				char *ptr;
				itmp.bv_val++;
				itmp.bv_len = bvl->bv.bv_len
					- ( itmp.bv_val - bvl->bv.bv_val );
				ptr = ber_bvchr( &itmp, IX_FSR );
				if ( ptr ) {
					itmp.bv_len = ptr - itmp.bv_val;
					ber_dupbv( &bvl->num, &itmp );
					bvl->inum = strtol( itmp.bv_val, NULL, 0 );
					itmp.bv_val[0] = '\0';
					bvl->off = itmp.bv_val - bvl->bv.bv_val;
				}
			}

			for (prev = &list; (ptr = *prev) != NULL; prev = &ptr->next) {
				int cmp = strcmp( bvl->bv.bv_val, ptr->bv.bv_val );
				if ( !cmp && bvl->num.bv_val )
					cmp = bvl->inum - ptr->inum;
				if ( cmp < 0 )
					break;
			}
			*prev = bvl;
			bvl->next = ptr;
				
		}
		closedir(dir_of_path);

		if (ck->op->ors_scope == LDAP_SCOPE_ONELEVEL)
			ck->op->ors_scope = LDAP_SCOPE_BASE;
		else if ( ck->op->ors_scope == LDAP_SCOPE_SUBORDINATE)
			ck->op->ors_scope = LDAP_SCOPE_SUBTREE;

		while ( ( ptr = list ) ) {
			struct berval fpath;

			list = ptr->next;

			if ( rc == LDAP_SUCCESS ) {
				if ( ptr->num.bv_val )
					AC_MEMCPY( ptr->bv.bv_val + ptr->off, ptr->num.bv_val,
						ptr->num.bv_len );
				fullpath( path, &ptr->bv, &fpath );
				rc = r_enum_tree(ck, &fpath, 0,
					e != NULL ? &e->e_name : pdn,
					e != NULL ? &e->e_nname : pndn );
				free(fpath.bv_val);
			}
			if ( ptr->num.bv_val )
				free( ptr->num.bv_val );
			free(ptr->bv.bv_val);
			free(ptr);
		}
	}
done:
	if ( fd ) entry_free( e );
	return rc;
}

static int
enum_tree(
	enumCookie *ck
)
{
	struct berval path;
	struct berval pdn, pndn;
	int rc;

	dnParent( &ck->op->o_req_dn, &pdn );
	dnParent( &ck->op->o_req_ndn, &pndn );
	dn2path( ck->op->o_bd, &ck->op->o_req_ndn, &path );
	rc = r_enum_tree(ck, &path, BER_BVISEMPTY( &ck->op->o_req_ndn ) ? 1 : 0, &pdn, &pndn);
	ch_free( path.bv_val );
	return rc;
}


/*
 * Prepare to create or rename an entry:
 * Check that the entry does not already exist.
 * Check that the parent entry exists and can have subordinates,
 * unless need_dir is NULL or adding the suffix entry.
 *
 * Return an LDAP result code.  May set *text to a message on failure.
 * If success, set *dnpath to LDIF entry path and *need_dir to
 * (directory must be created ? dirname : NULL).
 */
static int
ldif_prepare_create(
	Operation *op,
	Entry *e,
	struct berval *dnpath,
	char **need_dir,
	const char **text )
{
	BackendDB *be = op->o_bd;
	struct ldif_info *li = (struct ldif_info *) be->be_private;
	struct berval *ndn = &e->e_nname;
	struct berval ppath = BER_BVNULL;
	struct stat st;
	Entry *parent = NULL;
	int rc = LDAP_SUCCESS;

	dn2path( be, ndn, dnpath );

	if ( stat( dnpath->bv_val, &st ) == 0 ) { /* entry .ldif file */
		rc = LDAP_ALREADY_EXISTS;

	} else if ( errno != ENOENT ) {
		Debug( LDAP_DEBUG_ANY,
			"ldif_prepare_create: cannot stat \"%s\": %s\n",
			dnpath->bv_val, STRERROR( errno ), 0 );
		rc = LDAP_OTHER;
		*text = "internal error (cannot check entry file)";

	} else if ( need_dir != NULL ) {
		*need_dir = NULL;
		rc = get_parent_path( dnpath, &ppath );
		/* If parent dir exists, so does parent .ldif:
		 * The directory gets created after and removed before the .ldif.
		 * Except with the database directory, which has no matching entry.
		 */
		if ( rc == LDAP_SUCCESS && stat( ppath.bv_val, &st ) < 0 ) {
			rc = errno == ENOENT && ppath.bv_len > li->li_base_path.bv_len
				? LDAP_NO_SUCH_OBJECT : LDAP_OTHER;
		}
		switch ( rc ) {
		case LDAP_NO_SUCH_OBJECT:
			/* No parent dir, check parent .ldif */
			dir2ldif_name( ppath );
			rc = ldif_read_entry( op, ppath.bv_val, NULL, NULL,
				(op->o_tag != LDAP_REQ_ADD || get_manageDSAit( op )
				 ? &parent : NULL),
				text );
			switch ( rc ) {
			case LDAP_SUCCESS:
				/* Check that parent is not a referral, unless
				 * ldif_back_referrals() already checked.
				 */
				if ( parent != NULL ) {
					int is_ref = is_entry_referral( parent );
					entry_free( parent );
					if ( is_ref ) {
						rc = LDAP_AFFECTS_MULTIPLE_DSAS;
						*text = op->o_tag == LDAP_REQ_MODDN
							? "newSuperior is a referral object"
							: "parent is a referral object";
						break;
					}
				}
				/* Must create parent directory. */
				ldif2dir_name( ppath );
				*need_dir = ppath.bv_val;
				break;
			case LDAP_NO_SUCH_OBJECT:
				*text = op->o_tag == LDAP_REQ_MODDN
					? "newSuperior object does not exist"
					: "parent does not exist";
				break;
			}
			break;
		case LDAP_OTHER:
			Debug( LDAP_DEBUG_ANY,
				"ldif_prepare_create: cannot stat \"%s\" parent dir: %s\n",
				ndn->bv_val, STRERROR( errno ), 0 );
			*text = "internal error (cannot stat parent dir)";
			break;
		}
		if ( *need_dir == NULL && ppath.bv_val != NULL )
			SLAP_FREE( ppath.bv_val );
	}

	if ( rc != LDAP_SUCCESS ) {
		SLAP_FREE( dnpath->bv_val );
		BER_BVZERO( dnpath );
	}
	return rc;
}

static int apply_modify_to_entry(Entry * entry,
				Modifications * modlist,
				Operation * op,
				SlapReply * rs)
{
	char textbuf[SLAP_TEXT_BUFLEN];
	int rc = modlist ? LDAP_UNWILLING_TO_PERFORM : LDAP_SUCCESS;
	int is_oc = 0;
	Modification *mods;

	if (!acl_check_modlist(op, entry, modlist)) {
		return LDAP_INSUFFICIENT_ACCESS;
	}

	for (; modlist != NULL; modlist = modlist->sml_next) {
		mods = &modlist->sml_mod;

		if ( mods->sm_desc == slap_schema.si_ad_objectClass ) {
			is_oc = 1;
		}
		switch (mods->sm_op) {
		case LDAP_MOD_ADD:
			rc = modify_add_values(entry, mods,
				   get_permissiveModify(op),
				   &rs->sr_text, textbuf,
				   sizeof( textbuf ) );
			break;
				
		case LDAP_MOD_DELETE:
			rc = modify_delete_values(entry, mods,
				get_permissiveModify(op),
				&rs->sr_text, textbuf,
				sizeof( textbuf ) );
			break;
				
		case LDAP_MOD_REPLACE:
			rc = modify_replace_values(entry, mods,
				 get_permissiveModify(op),
				 &rs->sr_text, textbuf,
				 sizeof( textbuf ) );
			break;

		case LDAP_MOD_INCREMENT:
			rc = modify_increment_values( entry,
				mods, get_permissiveModify(op),
				&rs->sr_text, textbuf,
				sizeof( textbuf ) );
			break;

		case SLAP_MOD_SOFTADD:
			mods->sm_op = LDAP_MOD_ADD;
			rc = modify_add_values(entry, mods,
				   get_permissiveModify(op),
				   &rs->sr_text, textbuf,
				   sizeof( textbuf ) );
			mods->sm_op = SLAP_MOD_SOFTADD;
			if (rc == LDAP_TYPE_OR_VALUE_EXISTS) {
				rc = LDAP_SUCCESS;
			}
			break;
		}
		if(rc != LDAP_SUCCESS) break;
	}

	if ( rc == LDAP_SUCCESS ) {
		rs->sr_text = NULL; /* Needed at least with SLAP_MOD_SOFTADD */
		if ( is_oc ) {
			entry->e_ocflags = 0;
		}
		/* check that the entry still obeys the schema */
		rc = entry_schema_check( op, entry, NULL, 0, 0,
			  &rs->sr_text, textbuf, sizeof( textbuf ) );
	}

	return rc;
}


static int
ldif_back_referrals( Operation *op, SlapReply *rs )
{
	struct ldif_info *li = (struct ldif_info *) op->o_bd->be_private;
	struct berval path, dn = op->o_req_dn, ndn = op->o_req_ndn;
	ber_len_t min_dnlen;
	Entry *entry = NULL, **entryp;
	BerVarray ref;
	int rc;

	min_dnlen = op->o_bd->be_nsuffix[0].bv_len;
	if ( min_dnlen == 0 ) {
		/* Catch root DSE (empty DN), it is not a referral */
		min_dnlen = 1;
		if ( BER_BVISEMPTY( &ndn ) )
			return LDAP_SUCCESS;
	}

	entryp = get_manageDSAit( op ) ? NULL : &entry;
	dn2path( op->o_bd, &ndn, &path );
	ldap_pvt_thread_rdwr_rlock( &li->li_rdwr );

	for (;;) {
		dnParent( &dn, &dn );
		dnParent( &ndn, &ndn );
		rc = ldif_read_entry( op, path.bv_val, &dn, &ndn,
			entryp, &rs->sr_text );
		if ( rc != LDAP_NO_SUCH_OBJECT )
			break;

		rc = LDAP_SUCCESS;
		if ( ndn.bv_len < min_dnlen )
			break;
		(void) get_parent_path( &path, NULL );
		dir2ldif_name( path );
		entryp = &entry;
	}

	ldap_pvt_thread_rdwr_runlock( &li->li_rdwr );
	SLAP_FREE( path.bv_val );

	if ( entry != NULL ) {
		if ( is_entry_referral( entry ) ) {
			Debug( LDAP_DEBUG_TRACE,
				"ldif_back_referrals: tag=%lu target=\"%s\" matched=\"%s\"\n",
				(unsigned long) op->o_tag, op->o_req_dn.bv_val, entry->e_dn );

			ref = get_entry_referrals( op, entry );
			rs->sr_ref = referral_rewrite( ref, &entry->e_name, &op->o_req_dn,
				op->o_tag == LDAP_REQ_SEARCH ?
				op->ors_scope : LDAP_SCOPE_DEFAULT );
			ber_bvarray_free( ref );

			if ( rs->sr_ref != NULL ) {
				/* send referral */
				rc = rs->sr_err = LDAP_REFERRAL;
				rs->sr_matched = entry->e_dn;
				send_ldap_result( op, rs );
				ber_bvarray_free( rs->sr_ref );
				rs->sr_ref = NULL;
			} else {
				rc = LDAP_OTHER;
				rs->sr_text = "bad referral object";
			}
			rs->sr_matched = NULL;
		}

		entry_free( entry );
	}

	return rc;
}


/* LDAP operations */

static int
ldif_back_bind( Operation *op, SlapReply *rs )
{
	struct ldif_info *li;
	Attribute *a;
	AttributeDescription *password = slap_schema.si_ad_userPassword;
	int return_val;
	Entry *entry = NULL;

	switch ( be_rootdn_bind( op, rs ) ) {
	case SLAP_CB_CONTINUE:
		break;

	default:
		/* in case of success, front end will send result;
		 * otherwise, be_rootdn_bind() did */
		return rs->sr_err;
	}

	li = (struct ldif_info *) op->o_bd->be_private;
	ldap_pvt_thread_rdwr_rlock(&li->li_rdwr);
	return_val = get_entry(op, &entry, NULL, NULL);

	/* no object is found for them */
	if(return_val != LDAP_SUCCESS) {
		rs->sr_err = return_val = LDAP_INVALID_CREDENTIALS;
		goto return_result;
	}

	/* they don't have userpassword */
	if((a = attr_find(entry->e_attrs, password)) == NULL) {
		rs->sr_err = LDAP_INAPPROPRIATE_AUTH;
		return_val = 1;
		goto return_result;
	}

	/* authentication actually failed */
	if(slap_passwd_check(op, entry, a, &op->oq_bind.rb_cred,
			     &rs->sr_text) != 0) {
		rs->sr_err = LDAP_INVALID_CREDENTIALS;
		return_val = 1;
		goto return_result;
	}

	/* let the front-end send success */
	return_val = 0;
	goto return_result;

 return_result:
	ldap_pvt_thread_rdwr_runlock(&li->li_rdwr);
	if(return_val != 0)
		send_ldap_result( op, rs );
	if(entry != NULL)
		entry_free(entry);
	return return_val;
}

static int ldif_back_search(Operation *op, SlapReply *rs)
{
	struct ldif_info *li = (struct ldif_info *) op->o_bd->be_private;
	enumCookie ck = { NULL, NULL, NULL, 0, 0 };

	ck.op = op;
	ck.rs = rs;
	ldap_pvt_thread_rdwr_rlock(&li->li_rdwr);
	rs->sr_err = enum_tree( &ck );
	ldap_pvt_thread_rdwr_runlock(&li->li_rdwr);
	send_ldap_result(op, rs);

	return rs->sr_err;
}

static int ldif_back_add(Operation *op, SlapReply *rs) {
	struct ldif_info *li = (struct ldif_info *) op->o_bd->be_private;
	Entry * e = op->ora_e;
	struct berval path;
	char *parentdir;
	char textbuf[SLAP_TEXT_BUFLEN];
	int rc;

	Debug( LDAP_DEBUG_TRACE, "ldif_back_add: \"%s\"\n", e->e_dn, 0, 0 );

	rc = entry_schema_check( op, e, NULL, 0, 1,
		&rs->sr_text, textbuf, sizeof( textbuf ) );
	if ( rc != LDAP_SUCCESS )
		goto send_res;

	rc = slap_add_opattrs( op, &rs->sr_text, textbuf, sizeof( textbuf ), 1 );
	if ( rc != LDAP_SUCCESS )
		goto send_res;

	ldap_pvt_thread_rdwr_wlock(&li->li_rdwr);

	rc = ldif_prepare_create( op, e, &path, &parentdir, &rs->sr_text );
	if ( rc == LDAP_SUCCESS ) {
		rc = ldif_write_entry( op, e, &path, parentdir, &rs->sr_text );
		SLAP_FREE( path.bv_val );
		if ( parentdir != NULL )
			SLAP_FREE( parentdir );
	}

	ldap_pvt_thread_rdwr_wunlock(&li->li_rdwr);

 send_res:
	rs->sr_err = rc;
	Debug( LDAP_DEBUG_TRACE, "ldif_back_add: err: %d text: %s\n",
		rc, rs->sr_text ? rs->sr_text : "", 0 );
	send_ldap_result(op, rs);
	slap_graduate_commit_csn( op );
	return rs->sr_err;
}

static int ldif_back_modify(Operation *op, SlapReply *rs) {
	struct ldif_info *li = (struct ldif_info *) op->o_bd->be_private;
	Modifications * modlst = op->orm_modlist;
	struct berval path;
	Entry *entry;
	int rc;

	slap_mods_opattrs( op, &op->orm_modlist, 1 );

	ldap_pvt_thread_rdwr_wlock(&li->li_rdwr);

	rc = get_entry( op, &entry, &path, &rs->sr_text );
	if ( rc == LDAP_SUCCESS ) {
		rc = apply_modify_to_entry( entry, modlst, op, rs );
		if ( rc == LDAP_SUCCESS ) {
			rc = ldif_write_entry( op, entry, &path, NULL, &rs->sr_text );
		}

		entry_free( entry );
		SLAP_FREE( path.bv_val );
	}

	ldap_pvt_thread_rdwr_wunlock(&li->li_rdwr);

	rs->sr_err = rc;
	send_ldap_result( op, rs );
	slap_graduate_commit_csn( op );
	return rs->sr_err;
}

static int
ldif_back_delete( Operation *op, SlapReply *rs )
{
	struct ldif_info *li = (struct ldif_info *) op->o_bd->be_private;
	struct berval path;
	int rc = LDAP_SUCCESS;

	if ( BER_BVISEMPTY( &op->o_csn )) {
		struct berval csn;
		char csnbuf[LDAP_LUTIL_CSNSTR_BUFSIZE];

		csn.bv_val = csnbuf;
		csn.bv_len = sizeof( csnbuf );
		slap_get_csn( op, &csn, 1 );
	}

	ldap_pvt_thread_rdwr_wlock(&li->li_rdwr);

	dn2path( op->o_bd, &op->o_req_ndn, &path );
	ldif2dir_len( path );
	ldif2dir_name( path );
	if ( rmdir( path.bv_val ) < 0 ) {
		switch ( errno ) {
		case ENOTEMPTY:
			rc = LDAP_NOT_ALLOWED_ON_NONLEAF;
			break;
		case ENOENT:
			/* is leaf, go on */
			break;
		default:
			rc = LDAP_OTHER;
			rs->sr_text = "internal error (cannot delete subtree directory)";
			break;
		}
	}

	if ( rc == LDAP_SUCCESS ) {
		dir2ldif_name( path );
		if ( unlink( path.bv_val ) < 0 ) {
			rc = LDAP_NO_SUCH_OBJECT;
			if ( errno != ENOENT ) {
				rc = LDAP_OTHER;
				rs->sr_text = "internal error (cannot delete entry file)";
			}
		}
	}

	if ( rc == LDAP_OTHER ) {
		Debug( LDAP_DEBUG_ANY, "ldif_back_delete: %s \"%s\": %s\n",
			"cannot delete", path.bv_val, STRERROR( errno ) );
	}

	SLAP_FREE(path.bv_val);
	ldap_pvt_thread_rdwr_wunlock(&li->li_rdwr);
	rs->sr_err = rc;
	send_ldap_result( op, rs );
	slap_graduate_commit_csn( op );
	return rs->sr_err;
}


static int
ldif_move_entry(
	Operation *op,
	Entry *entry,
	struct berval *oldpath,
	const char **text )
{
	struct berval newpath;
	char *parentdir = NULL, *trash;
	int rc, rename_res;

	rc = ldif_prepare_create( op, entry, &newpath,
			op->orr_newSup ? &parentdir : NULL, text );

	if ( rc == LDAP_SUCCESS ) {
		rc = ldif_write_entry( op, entry, &newpath, parentdir, text );
		if ( rc == LDAP_SUCCESS ) {
			trash = oldpath->bv_val; /* will be .ldif file to delete */
			ldif2dir_len( newpath );
			ldif2dir_len( *oldpath );
			/* Move subdir before deleting old entry,
			 * so .ldif always exists if subdir does.
			 */
			ldif2dir_name( newpath );
			ldif2dir_name( *oldpath );
			rename_res = move_dir( oldpath->bv_val, newpath.bv_val );
			if ( rename_res != 0 && errno != ENOENT ) {
				rc = LDAP_OTHER;
				*text = "internal error (cannot move this subtree)";
				trash = newpath.bv_val;
			}

			/* Delete old entry, or if error undo change */
			for (;;) {
				dir2ldif_name( newpath );
				dir2ldif_name( *oldpath );
				if ( unlink( trash ) == 0 )
					break;
				if ( rc == LDAP_SUCCESS ) {
					/* Prepare to undo change and return failure */
					rc = LDAP_OTHER;
					*text = "internal error (cannot move this entry)";
					trash = newpath.bv_val;
					if ( rename_res != 0 )
						continue;
					/* First move subdirectory back */
					ldif2dir_name( newpath );
					ldif2dir_name( *oldpath );
					if ( move_dir( newpath.bv_val, oldpath->bv_val ) == 0 )
						continue;
				}
				*text = "added new but couldn't delete old entry!";
				break;
			}

			if ( rc != LDAP_SUCCESS ) {
				char s[128];
				snprintf( s, sizeof s, "%s (%s)", *text, STRERROR( errno ));
				Debug( LDAP_DEBUG_ANY,
					"ldif_move_entry: %s: \"%s\" -> \"%s\"\n",
					s, op->o_req_dn.bv_val, entry->e_dn );
			}
		}

		SLAP_FREE( newpath.bv_val );
		if ( parentdir != NULL )
			SLAP_FREE( parentdir );
	}

	return rc;
}

static int
ldif_back_modrdn(Operation *op, SlapReply *rs)
{
	struct ldif_info *li = (struct ldif_info *) op->o_bd->be_private;
	struct berval new_dn = BER_BVNULL, new_ndn = BER_BVNULL;
	struct berval p_dn, old_path;
	Entry *entry;
	int rc;

	slap_mods_opattrs( op, &op->orr_modlist, 1 );

	ldap_pvt_thread_rdwr_wlock( &li->li_rdwr );

	rc = get_entry( op, &entry, &old_path, &rs->sr_text );
	if ( rc == LDAP_SUCCESS ) {
		/* build new dn, and new ndn for the entry */
		if ( op->oq_modrdn.rs_newSup != NULL ) {
			p_dn = *op->oq_modrdn.rs_newSup;
		} else {
			dnParent( &entry->e_name, &p_dn );
		}
		build_new_dn( &new_dn, &p_dn, &op->oq_modrdn.rs_newrdn, NULL ); 
		dnNormalize( 0, NULL, NULL, &new_dn, &new_ndn, NULL );
		ber_memfree_x( entry->e_name.bv_val, NULL );
		ber_memfree_x( entry->e_nname.bv_val, NULL );
		entry->e_name = new_dn;
		entry->e_nname = new_ndn;

		/* perform the modifications */
		rc = apply_modify_to_entry( entry, op->orr_modlist, op, rs );
		if ( rc == LDAP_SUCCESS )
			rc = ldif_move_entry( op, entry, &old_path, &rs->sr_text );

		entry_free( entry );
		SLAP_FREE( old_path.bv_val );
	}

	ldap_pvt_thread_rdwr_wunlock( &li->li_rdwr );
	rs->sr_err = rc;
	send_ldap_result( op, rs );
	slap_graduate_commit_csn( op );
	return rs->sr_err;
}


/* Return LDAP_SUCCESS IFF we retrieve the specified entry. */
static int
ldif_back_entry_get(
	Operation *op,
	struct berval *ndn,
	ObjectClass *oc,
	AttributeDescription *at,
	int rw,
	Entry **e )
{
	struct ldif_info *li = (struct ldif_info *) op->o_bd->be_private;
	struct berval op_dn = op->o_req_dn, op_ndn = op->o_req_ndn;
	int rc;

	assert( ndn != NULL );
	assert( !BER_BVISNULL( ndn ) );

	ldap_pvt_thread_rdwr_rlock( &li->li_rdwr );
	op->o_req_dn = *ndn;
	op->o_req_ndn = *ndn;
	rc = get_entry( op, e, NULL, NULL );
	op->o_req_dn = op_dn;
	op->o_req_ndn = op_ndn;
	ldap_pvt_thread_rdwr_runlock( &li->li_rdwr );

	if ( rc == LDAP_SUCCESS && oc && !is_entry_objectclass_or_sub( *e, oc ) ) {
		rc = LDAP_NO_SUCH_ATTRIBUTE;
		entry_free( *e );
		*e = NULL;
	}

	return rc;
}


/* Slap tools */

static int ldif_tool_entry_open(BackendDB *be, int mode) {
	struct ldif_info *li = (struct ldif_info *) be->be_private;
	li->li_tool_current = 0;
	return 0;
}					

static int ldif_tool_entry_close(BackendDB * be) {
	struct ldif_info *li = (struct ldif_info *) be->be_private;

	SLAP_FREE(li->li_tool_cookie.entries);
	return 0;
}

static ID ldif_tool_entry_next(BackendDB *be)
{
	struct ldif_info *li = (struct ldif_info *) be->be_private;
	if(li->li_tool_current >= li->li_tool_cookie.eind)
		return NOID;
	else
		return ++li->li_tool_current;
}

static ID
ldif_tool_entry_first(BackendDB *be)
{
	struct ldif_info *li = (struct ldif_info *) be->be_private;

	if(li->li_tool_cookie.entries == NULL) {
		Operation op = {0};

		op.o_bd = be;
		op.o_req_dn = *be->be_suffix;
		op.o_req_ndn = *be->be_nsuffix;
		op.ors_scope = LDAP_SCOPE_SUBTREE;
		li->li_tool_cookie.op = &op;
		(void)enum_tree( &li->li_tool_cookie );
		li->li_tool_cookie.op = NULL;
	}
	return ldif_tool_entry_next( be );
}

static Entry * ldif_tool_entry_get(BackendDB * be, ID id) {
	struct ldif_info *li = (struct ldif_info *) be->be_private;
	Entry * e;

	if(id > li->li_tool_cookie.eind || id < 1)
		return NULL;
	else {
		e = li->li_tool_cookie.entries[id - 1];
		li->li_tool_cookie.entries[id - 1] = NULL;
		return e;
	}
}

static ID
ldif_tool_entry_put( BackendDB *be, Entry *e, struct berval *text )
{
	int rc;
	const char *errmsg = NULL;
	struct berval path;
	char *parentdir;
	Operation op = {0};

	op.o_bd = be;
	rc = ldif_prepare_create( &op, e, &path, &parentdir, &errmsg );
	if ( rc == LDAP_SUCCESS ) {
		rc = ldif_write_entry( &op, e, &path, parentdir, &errmsg );

		SLAP_FREE( path.bv_val );
		if ( parentdir != NULL )
			SLAP_FREE( parentdir );
		if ( rc == LDAP_SUCCESS )
			return 1;
	}

	if ( errmsg == NULL && rc != LDAP_OTHER )
		errmsg = ldap_err2string( rc );
	if ( errmsg != NULL )
		snprintf( text->bv_val, text->bv_len, "%s", errmsg );
	return NOID;
}


/* Setup */

static int
ldif_back_db_init( BackendDB *be, ConfigReply *cr )
{
	struct ldif_info *li;

	li = ch_calloc( 1, sizeof(struct ldif_info) );
	be->be_private = li;
	be->be_cf_ocs = ldifocs;
	ldap_pvt_thread_rdwr_init(&li->li_rdwr);
	SLAP_DBFLAGS( be ) |= SLAP_DBFLAG_ONE_SUFFIX;
	return 0;
}

static int
ldif_back_db_destroy( Backend *be, ConfigReply *cr )
{
	struct ldif_info *li = be->be_private;

	ch_free(li->li_base_path.bv_val);
	ldap_pvt_thread_rdwr_destroy(&li->li_rdwr);
	free( be->be_private );
	return 0;
}

static int
ldif_back_db_open( Backend *be, ConfigReply *cr)
{
	struct ldif_info *li = (struct ldif_info *) be->be_private;
	if( BER_BVISEMPTY(&li->li_base_path)) {/* missing base path */
		Debug( LDAP_DEBUG_ANY, "missing base path for back-ldif\n", 0, 0, 0);
		return 1;
	}
	return 0;
}

int
ldif_back_initialize(
			   BackendInfo	*bi
			   )
{
	static char *controls[] = {
		LDAP_CONTROL_MANAGEDSAIT,
		NULL
	};
	int rc;

	bi->bi_flags |=
		SLAP_BFLAG_INCREMENT |
		SLAP_BFLAG_REFERRALS;

	bi->bi_controls = controls;

	bi->bi_open = 0;
	bi->bi_close = 0;
	bi->bi_config = 0;
	bi->bi_destroy = 0;

	bi->bi_db_init = ldif_back_db_init;
	bi->bi_db_config = config_generic_wrapper;
	bi->bi_db_open = ldif_back_db_open;
	bi->bi_db_close = 0;
	bi->bi_db_destroy = ldif_back_db_destroy;

	bi->bi_op_bind = ldif_back_bind;
	bi->bi_op_unbind = 0;
	bi->bi_op_search = ldif_back_search;
	bi->bi_op_compare = 0;
	bi->bi_op_modify = ldif_back_modify;
	bi->bi_op_modrdn = ldif_back_modrdn;
	bi->bi_op_add = ldif_back_add;
	bi->bi_op_delete = ldif_back_delete;
	bi->bi_op_abandon = 0;

	bi->bi_extended = 0;

	bi->bi_chk_referrals = ldif_back_referrals;

	bi->bi_connection_init = 0;
	bi->bi_connection_destroy = 0;

	bi->bi_entry_get_rw = ldif_back_entry_get;

#if 0	/* NOTE: uncomment to completely disable access control */
	bi->bi_access_allowed = slap_access_always_allowed;
#endif

	bi->bi_tool_entry_open = ldif_tool_entry_open;
	bi->bi_tool_entry_close = ldif_tool_entry_close;
	bi->bi_tool_entry_first = ldif_tool_entry_first;
	bi->bi_tool_entry_next = ldif_tool_entry_next;
	bi->bi_tool_entry_get = ldif_tool_entry_get;
	bi->bi_tool_entry_put = ldif_tool_entry_put;
	bi->bi_tool_entry_reindex = 0;
	bi->bi_tool_sync = 0;
	
	bi->bi_tool_dn2id_get = 0;
	bi->bi_tool_entry_modify = 0;

	bi->bi_cf_ocs = ldifocs;

	rc = config_register_schema( ldifcfg, ldifocs );
	if ( rc ) return rc;
	return 0;
}
