/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __las_drop --
 *	Discard the database's lookaside store.
 */
static int
__las_drop(WT_SESSION_IMPL *session)
{
	const char *drop_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_drop), "force=true", NULL };

	return (__wt_session_drop(session, WT_LASFILE_URI, drop_cfg));
}

/*
 * __wt_las_create --
 *	Initialize the database's lookaside store.
 */
int
__wt_las_create(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	const char *open_cursor_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_open_cursor),
	    "overwrite=false", NULL };

	conn = S2C(session);

	/* Lock the lookaside table and check if we won the race. */
	__wt_spin_lock(session, &conn->las_lock);
	if (conn->las_cursor != NULL) {
		__wt_spin_unlock(session, &conn->las_lock);
		return (0);
	}

	/* Open an internal session, used for lookaside cursors. */
	WT_ERR(__wt_open_internal_session(
	    conn, "lookaside table", 1, 1, &conn->las_session));
	session = conn->las_session;

	/* Discard any previous incarnation of the file. */
	WT_ERR(__las_drop(session));

	/* Re-create the file. */
	WT_ERR(__wt_session_create(
	    session, WT_LASFILE_URI, "key_format=u,value_format=QIu"));

	/*
	 * Open the cursor. (Note the "overwrite=false" configuration, we want
	 * to see errors if we try to remove records that aren't there.)
	 */
	WT_ERR(__wt_open_cursor(
	    session, WT_LASFILE_URI, NULL, open_cursor_cfg, &conn->las_cursor));

	/*
	 * No eviction.
	 * No lookaside records during reconciliation.
	 * No checkpoints or logging.
	 */
	F_SET(session, WT_SESSION_NO_EVICTION);
	F_SET(S2BT(session),
	    WT_BTREE_LAS_FILE | WT_BTREE_NO_CHECKPOINT | WT_BTREE_NO_LOGGING);

err:	__wt_spin_unlock(session, &conn->las_lock);
	return (ret);
}

/*
 * __wt_las_destroy --
 *	Destroy the database's lookaside store.
 */
int
__wt_las_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	if (conn->las_session == NULL)
		return (0);

	/* Close the session, closing the open cursor. */
	wt_session = &conn->las_session->iface;
	WT_TRET(wt_session->close(wt_session, NULL));

	/*
	 * Clear the references (this isn't just for clarity, the underlying
	 * code uses the non-NULL cursor to determine if information in the
	 * lookaside table needs to be updated as blocks are freed).
	 *
	 * KEITH:
	 * This isn't right; we should be setting WT_SYNC_DISCARD_FORCE and
	 * simply discarding the dirty blocks before dropping the file.
	 */
	conn->las_cursor = NULL;
	conn->las_session = NULL;

	/* Discard any incarnation of the file. */
	WT_TRET(__las_drop(conn->las_session));

	return (ret);
}

/*
 * __wt_las_cursor --
 *	Return a lookaside cursor.
 */
int
__wt_las_cursor(
    WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t *saved_flagsp)
{
	WT_CONNECTION_IMPL *conn;

	*cursorp = NULL;
	*saved_flagsp = session->flags;

	conn = S2C(session);

	/* On the first access, create the lookaside store and cursor. */
	if (conn->las_cursor == NULL)
		WT_RET(__wt_las_create(session));

	__wt_spin_lock(session, &conn->las_lock);

	F_SET(session, WT_SESSION_NO_EVICTION);

	*cursorp = conn->las_cursor;

	return (0);
}

/*
 * __wt_las_cursor_close --
 *	Discard a lookaside cursor.
 */
int
__wt_las_cursor_close(
	WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t saved_flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	conn = S2C(session);

	if ((cursor = *cursorp) == NULL)
		return (0);
	*cursorp = NULL;
	session->flags = saved_flags;

	/* Reset the cursor. */
	ret = cursor->reset(cursor);

	__wt_spin_unlock(session, &conn->las_lock);

	return (ret);
}
