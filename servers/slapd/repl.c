/* repl.c - log modifications for replication purposes */
/* $OpenLDAP$ */
/*
 * Copyright 1998-2000 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/ctype.h>
#include <ac/socket.h>

#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif

#include "slap.h"
#include "ldif.h"

int
add_replica_info(
    Backend     *be,
    const char  *host 
)
{
	int i = 0;

	assert( be );
	assert( host );

	if ( be->be_replica != NULL ) {
		for ( ; be->be_replica[ i ] != NULL; i++ );
	}
		
	be->be_replica = ch_realloc( be->be_replica, 
		sizeof( struct slap_replica_info * )*( i + 2 ) );

	be->be_replica[ i ] 
		= ch_calloc( sizeof( struct slap_replica_info ), 1 );
	be->be_replica[ i ]->ri_host = ch_strdup( host );
	be->be_replica[ i + 1 ] = NULL;

	return( i );
}

int
add_replica_suffix(
    Backend     *be,
    int		nr,
    const char  *suffix
)
{
	struct berval dn, *ndn = NULL;
	int rc;

	dn.bv_val = (char *) suffix;
	dn.bv_len = strlen( dn.bv_val );

	rc = dnNormalize( NULL, &dn, &ndn );
	if( rc != LDAP_SUCCESS ) {
		return 2;
	}

	if ( select_backend( ndn, 0, 0 ) != be ) {
		ber_bvfree( ndn );
		return 1;
	}

	ber_bvecadd( &be->be_replica[nr]->ri_nsuffix, ndn );
	return 0;
}

void
replog(
    Backend	*be,
    Operation *op,
    struct berval *dn,
    struct berval *ndn,
    void	*change
)
{
	Modifications	*ml;
	Entry	*e;
	struct slap_replog_moddn *moddn;
	char *tmp;
	FILE	*fp, *lfp;
	int	len, i;
/* undef NO_LOG_WHEN_NO_REPLICAS */
#ifdef NO_LOG_WHEN_NO_REPLICAS
	int     count = 0;
#endif

	if ( be->be_replogfile == NULL && replogfile == NULL ) {
		return;
	}

	ldap_pvt_thread_mutex_lock( &replog_mutex );
	if ( (fp = lock_fopen( be->be_replogfile ? be->be_replogfile :
	    replogfile, "a", &lfp )) == NULL ) {
		ldap_pvt_thread_mutex_unlock( &replog_mutex );
		return;
	}

	for ( i = 0; be->be_replica != NULL && be->be_replica[i] != NULL; i++ ) {
		/* check if dn's suffix matches legal suffixes, if any */
		if ( be->be_replica[i]->ri_nsuffix != NULL ) {
			int j;

			for ( j = 0; be->be_replica[i]->ri_nsuffix[j]; j++ ) {
				if ( dnIsSuffix( ndn, be->be_replica[i]->ri_nsuffix[j] ) ) {
					break;
				}
			}

			if ( !be->be_replica[i]->ri_nsuffix[j] ) {
				/* do not add "replica:" line */
				continue;
			}
		}

		fprintf( fp, "replica: %s\n", be->be_replica[i]->ri_host );
#ifdef NO_LOG_WHEN_NO_REPLICAS
		++count;
#endif
	}

#ifdef NO_LOG_WHEN_NO_REPLICAS
	if ( count == 0 ) {
		/* if no replicas matched, drop the log 
		 * (should we log it anyway?) */
		lock_fclose( fp, lfp );
		ldap_pvt_thread_mutex_unlock( &replog_mutex );

		return;
	}
#endif

	fprintf( fp, "time: %ld\n", (long) slap_get_time() );
	fprintf( fp, "dn: %s\n", dn->bv_val );

	switch ( op->o_tag ) {
	case LDAP_REQ_EXTENDED:
		/* quick hack for extended operations */
		/* assume change parameter is a Modfications* */
		/* fall thru */

	case LDAP_REQ_MODIFY:
		fprintf( fp, "changetype: modify\n" );
		ml = change;
		for ( ; ml != NULL; ml = ml->sml_next ) {
			char *type;
			type = ml->sml_desc->ad_cname.bv_val;
			switch ( ml->sml_op ) {
			case LDAP_MOD_ADD:
				fprintf( fp, "add: %s\n", type );
				break;

			case LDAP_MOD_DELETE:
				fprintf( fp, "delete: %s\n", type );
				break;

			case LDAP_MOD_REPLACE:
				fprintf( fp, "replace: %s\n", type );
				break;
			}

			for ( i = 0; ml->sml_bvalues != NULL &&
			    ml->sml_bvalues[i] != NULL; i++ )
			{
				char	*buf, *bufp;

				len = ml->sml_desc->ad_cname.bv_len;
				len = LDIF_SIZE_NEEDED( len,
				    ml->sml_bvalues[i]->bv_len ) + 1;
				buf = (char *) ch_malloc( len );

				bufp = buf;
				ldif_sput( &bufp, LDIF_PUT_VALUE, type,
				    ml->sml_bvalues[i]->bv_val,
				    ml->sml_bvalues[i]->bv_len );
				*bufp = '\0';

				fputs( buf, fp );

				free( buf );
			}
			fprintf( fp, "-\n" );
		}
		break;

	case LDAP_REQ_ADD:
		e = change;
		fprintf( fp, "changetype: add\n" );
		ldap_pvt_thread_mutex_lock( &entry2str_mutex );
		tmp = entry2str( e, &len );
		while ( (tmp = strchr( tmp, '\n' )) != NULL ) {
			tmp++;
			if ( ! isspace( (unsigned char) *tmp ) )
				break;
		}
		fprintf( fp, "%s", tmp );
		ldap_pvt_thread_mutex_unlock( &entry2str_mutex );
		break;

	case LDAP_REQ_DELETE:
		fprintf( fp, "changetype: delete\n" );
		break;

	case LDAP_REQ_MODRDN:
		moddn = change;
		fprintf( fp, "changetype: modrdn\n" );
		fprintf( fp, "newrdn: %s\n", moddn->newrdn->bv_val );
		fprintf( fp, "deleteoldrdn: %d\n", moddn->deloldrdn ? 1 : 0 );
		if( moddn->newsup != NULL ) {
			fprintf( fp, "newsuperior: %s\n", moddn->newsup->bv_val );
		}
	}
	fprintf( fp, "\n" );

	lock_fclose( fp, lfp );
	ldap_pvt_thread_mutex_unlock( &replog_mutex );
}
