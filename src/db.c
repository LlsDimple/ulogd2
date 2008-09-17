/* db.c
 *
 * ulogd helper functions for Database / SQL output plugins
 *
 * (C) 2000-2005 by Harald Welte <laforge@gnumonks.org>
 *
 *  Portions (C) 2001 Alex Janssen <alex@ynfonatic.de>,
 *           (C) 2005 Sven Schuster <schuster.sven@gmx.de>,
 *           (C) 2005 Jozsef Kadlecsik <kadlec@blackhole.kfki.hu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <ulogd/ulogd.h>
#include <ulogd/common.h>
#include <ulogd/db.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* generic db layer */

/* this is a wrapper that just calls the current real
 * interp function */
int
ulogd_db_interp(struct ulogd_pluginstance *upi)
{
	struct db_instance *dbi = upi_priv(upi);

	pr_debug("%s: upi=%p\n", __func__, upi);

	assert(dbi->interp != NULL);

	return dbi->interp(upi);
}

/* no connection, plugin disabled */
static int
disabled_interp_db(struct ulogd_pluginstance *upi)
{
	pr_debug("%s: upi=%p\n", __func__, upi);

	return 0;
}

#define SQL_INSERTTEMPL   "insert into X (Y) values (Z)"
#define SQL_VALSIZE	100

/* create the static part of our insert statement */
static int
sql_createstmt(struct ulogd_pluginstance *upi)
{
	struct db_instance *mi = upi_priv(upi);
	unsigned int size;
	char *table = table_ce(upi->config_kset).u.string;
	int i;

	pr_debug("%s: upi=%p\n", __func__, upi);

	if (mi->stmt)
		free(mi->stmt);

	/* caclulate the size for the insert statement */
	size = strlen(SQL_INSERTTEMPL) + strlen(table);

	for (i = 0; i < upi->input.num_keys; i++) {
		if (upi->input.keys[i].flags & ULOGD_KEYF_INACTIVE)
			continue;
		/* we need space for the key and a comma, as well as
		 * enough space for the values */
		size += strlen(upi->input.keys[i].name) + 1 + SQL_VALSIZE;
	}

	ulogd_log(ULOGD_DEBUG, "allocating %u bytes for statement\n", size);

	mi->stmt = (char *) malloc(size);
	if (!mi->stmt) {
		ulogd_log(ULOGD_ERROR, "OOM!\n");
		return -ENOMEM;
	}

	if (mi->schema)
		sprintf(mi->stmt, "insert into %s.%s (", mi->schema, table);
	else
		sprintf(mi->stmt, "insert into %s (", table);

	mi->stmt_val = mi->stmt + strlen(mi->stmt);

	for (i = 0; i < upi->input.num_keys; i++) {
		if (upi->input.keys[i].flags & ULOGD_KEYF_INACTIVE)
			continue;

		strncpy(mi->stmt_val, upi->input.keys[i].name, ULOGD_MAX_KEYLEN);
		strntr(mi->stmt_val, '.', '_');

		mi->stmt_val += strlen(upi->input.keys[i].name);

		if (i + 1 < upi->input.num_keys)
			*(mi->stmt_val)++ = ',';
	}

	*(mi->stmt_val)++ = ')';
	mi->stmt_val += sprintf(mi->stmt_val, " values (");

	ulogd_log(ULOGD_DEBUG, "stmt='%s'\n", mi->stmt);

	return 0;
}

static int
check_driver(struct ulogd_pluginstance *pi)
{
	const struct db_instance *di = upi_priv(pi);
	const struct db_driver *drv = di->driver;

	if (drv->open_db == NULL || drv->close_db == NULL
		|| drv->get_columns == NULL)
		return -1;

	if (drv->commit == NULL)
		return -1;

	return 0;
}

int
ulogd_db_configure(struct ulogd_pluginstance *upi,
				   struct ulogd_pluginstance_stack *stack)
{
	struct db_instance *di = upi_priv(upi);
	int ret;

	pr_debug("%s: upi=%p\n", __func__, upi);

	ulogd_log(ULOGD_NOTICE, "(re)configuring\n");

	if (check_driver(upi) < 0)
		return -1;

	/* First: Parse configuration file section for this instance */
	ret = config_parse_file(upi->id, upi->config_kset);
	if (ret < 0) {
		ulogd_log(ULOGD_ERROR, "error parsing config file\n");
		return ret;
	}

	/* TODO make configurable */
	di->buffer_size = 1;
	di->max_backlog = 1024 * 1024;

	/* Second: Open Database */
	ret = di->driver->open_db(upi);
	if (ret < 0) {
		ulogd_log(ULOGD_ERROR, "error in open_db\n");
		return ret;
	}

	/* Third: Determine required input keys for given table */
	ret = di->driver->get_columns(upi);
	if (ret < 0)
		ulogd_log(ULOGD_ERROR, "error in get_columns\n");

	/* Close database, since ulogd core could just call configure
	 * but abort during input key resolving routines.  configure
	 * doesn't have a destructor... */
	di->driver->close_db(upi);

	return ret;
}

static int _init_db(struct ulogd_pluginstance *upi);

int
ulogd_db_start(struct ulogd_pluginstance *upi)
{
	struct db_instance *di = upi_priv(upi);

	pr_debug("%s: upi=%p\n", __func__, upi);
	ulogd_log(ULOGD_NOTICE, "starting\n");

	if (di->driver->open_db(upi) < 0)
		return -1;

	if (db_has_prepare(di)) {
		di->driver->prepare(upi); /* TODO check retval */
	} else if (sql_createstmt(upi) < 0)
		goto err_close;

	INIT_LLIST_HEAD(&di->rows);
	INIT_LLIST_HEAD(&di->rows_committed);
	di->num_rows = 0;

	/* note that this handler is only used for those DB plugins which
	   use ulogd_db_interp(), others use their own handler (such
	   as pgsql). */
	di->interp = _init_db;

	return 0;

err_close:
	di->driver->close_db(upi);

	return -1;
}

int
ulogd_db_stop(struct ulogd_pluginstance *upi)
{
	struct db_instance *di = upi_priv(upi);

	pr_debug("%s: upi=%p\n", __func__, upi);

	ulogd_log(ULOGD_NOTICE, "stopping\n");
	di->driver->close_db(upi);

	/* try to free our dynamically allocated input key array */
	if (upi->input.keys) {
		free(upi->input.keys);
		upi->input.keys = NULL;
	}
	return 0;
}

static int
_init_reconnect(struct ulogd_pluginstance *upi)
{
	struct db_instance *di = upi_priv(upi);

	pr_debug("%s: upi=%p\n", __func__, upi);

	if (reconnect_ce(upi->config_kset).u.value) {
		di->reconnect = time(NULL);
		if (di->reconnect != TIME_ERR) {
			ulogd_log(ULOGD_ERROR, "no connection to database, "
				  "attempting to reconnect after %u seconds\n",
				  reconnect_ce(upi->config_kset).u.value);
			di->reconnect += reconnect_ce(upi->config_kset).u.value;
			di->interp = &_init_db;
			return -1;
		}
	}

	/* Disable plugin permanently */
	ulogd_log(ULOGD_ERROR, "permanently disabling plugin\n");
	di->interp = &disabled_interp_db;

	return 0;
}

/* our main output function, called by ulogd if ulogd_db_interp() is
   set as interpreter function in the plugin. */
static int
__interp_db(struct ulogd_pluginstance *upi)
{
	struct db_instance *di = upi_priv(upi);
	int i;

	pr_debug("%s: upi=%p\n", __func__, upi);

	di->stmt_ins = di->stmt_val;

	for (i = 0; i < upi->input.num_keys; i++) {
		struct ulogd_key *res = upi->input.keys[i].u.source;

		if (upi->input.keys[i].flags & ULOGD_KEYF_INACTIVE)
			continue;

		if (!res)
			ulogd_log(ULOGD_NOTICE, "no source for `%s' ?!?\n",
				  upi->input.keys[i].name);

		if (!res || !IS_VALID(*res)) {
			/* no result, we have to fake something */
			di->stmt_ins += sprintf(di->stmt_ins, "NULL,");
			continue;
		}

		switch (res->type) {
			char *tmpstr;
			struct in_addr addr;
		case ULOGD_RET_INT8:
			sprintf(di->stmt_ins, "%d,", res->u.value.i8);
			break;
		case ULOGD_RET_INT16:
			sprintf(di->stmt_ins, "%d,", res->u.value.i16);
			break;
		case ULOGD_RET_INT32:
			sprintf(di->stmt_ins, "%d,", res->u.value.i32);
			break;
		case ULOGD_RET_INT64:
			sprintf(di->stmt_ins, "%lld,", res->u.value.i64);
			break;
		case ULOGD_RET_UINT8:
			sprintf(di->stmt_ins, "%u,", res->u.value.ui8);
			break;
		case ULOGD_RET_UINT16:
			sprintf(di->stmt_ins, "%u,", res->u.value.ui16);
			break;
		case ULOGD_RET_IPADDR:
			if (asstring_ce(upi->config_kset).u.value) {
				memset(&addr, 0, sizeof(addr));
				addr.s_addr = ntohl(res->u.value.ui32);
				*(di->stmt_ins++) = '\'';
				tmpstr = inet_ntoa(addr);
				di->driver->escape_string(upi, di->stmt_ins,
							  tmpstr, strlen(tmpstr));
                                di->stmt_ins = di->stmt + strlen(di->stmt);
				sprintf(di->stmt_ins, "',");
				break;
			}
			/* fallthrough when logging IP as u_int32_t */
		case ULOGD_RET_UINT32:
			sprintf(di->stmt_ins, "%u,", res->u.value.ui32);
			break;
		case ULOGD_RET_UINT64:
			sprintf(di->stmt_ins, "%llu,", res->u.value.ui64);
			break;
		case ULOGD_RET_BOOL:
			sprintf(di->stmt_ins, "'%d',", res->u.value.b);
			break;
		case ULOGD_RET_STRING:
			*(di->stmt_ins++) = '\'';
			if (res->u.value.ptr) {
				di->stmt_ins +=
				di->driver->escape_string(upi, di->stmt_ins,
							  res->u.value.ptr,
							strlen(res->u.value.ptr));
			}
			sprintf(di->stmt_ins, "',");
			break;
		case ULOGD_RET_RAW:
			ulogd_log(ULOGD_NOTICE,
				"%s: type RAW not supported by MySQL\n",
				upi->input.keys[i].name);
			break;
		default:
			ulogd_log(ULOGD_NOTICE,
				"unknown type %d for %s\n",
				res->type, upi->input.keys[i].name);
			break;
		}
		di->stmt_ins = di->stmt + strlen(di->stmt);
	}
	*(di->stmt_ins - 1) = ')';

	/* now we have created our statement, insert it */

	if (di->driver->execute(upi, di->stmt, strlen(di->stmt)) < 0)
		return _init_db(upi);

	return 0;
}

static int
_init_db(struct ulogd_pluginstance *upi)
{
	struct db_instance *di = upi_priv(upi);

	pr_debug("%s: upi=%p\n", __func__, upi);

	if (di->reconnect && di->reconnect > time(NULL))
		return 0;

	if (di->driver->open_db(upi)) {
		ulogd_log(ULOGD_ERROR, "can't establish database connection\n");
		return _init_reconnect(upi);
	}

	/* The di->interp hook function is only called from ulogd_db_interp()
	 * nowadays.  Plugins with more advanced commit logic (prepared
	 * statements, batching, ...) have their own handler. */
	di->interp = __interp_db;

	di->reconnect = 0;

	/* call the interpreter function to actually write the
	 * log line that we wanted to write */
	return di->interp(upi);
}

void
ulogd_db_signal(struct ulogd_pluginstance *upi, int signal)
{
	switch (signal) {
	case SIGHUP:
		/* reopen database connection */
		ulogd_db_stop(upi);
		ulogd_db_start(upi);
		break;
	default:
		break;
	}
}

struct db_row *
db_row_new(struct ulogd_pluginstance *pi)
{
	struct db_instance *di = upi_priv(pi);
	struct db_row *row;

	pr_fn_debug("pi=%p\n", pi);

	if ((row = calloc(1, sizeof(struct db_row))) == NULL) {
		ulogd_log(ULOGD_FATAL, "db: out of memory\n");
		return NULL;
	}

	di->num_rows++;

	return row;
}

static void
__db_row_del(struct ulogd_pluginstance *pi, struct db_row *row)
{
	pr_fn_debug("pi=%p row=%p\n", pi, row);

	if (row != NULL) {
		struct db_instance *di = upi_priv(pi);

		free(row);

		di->num_rows--;
	}
}

void
db_row_del(struct ulogd_pluginstance *pi, struct db_row *row)
{
	pr_fn_debug("pi=%p row=%p\n", pi, row);

	llist_del(&row->link);

	__db_row_del(pi, row);
}

int
db_row_add(struct ulogd_pluginstance *pi, struct db_row *row)
{
	struct db_instance *di = upi_priv(pi);

	pr_fn_debug("pi=%p row=%p\n", pi, row);

	if (di->max_backlog && di->num_rows >= di->max_backlog) {
		if (!di->overlimit_msg) {
			ulogd_log(ULOGD_ERROR, "db: over backlog limit, dropping rows\n");
			di->overlimit_msg = 1;
		}

		__db_row_del(pi, row);

		return -1;
	}

	llist_add(&row->link, &di->rows);

	di->num_rows++;

	return 0;
}

static int
__db_commit(struct ulogd_pluginstance *pi)
{
	struct db_instance *di = upi_priv(pi);
	int max_commit;

	pr_fn_debug("pi=%p\n", pi);

	/* Limit number of rows to commit.  Note that currently three times
	   buffer_size is a bit arbitrary and therefore might be adjusted in
	   the future. */
	max_commit = max(3 * di->buffer_size, 1024);

	if (di->driver->commit(pi, max_commit) < 0) {
		ulogd_log(ULOGD_ERROR, "%s: commit failed\n", pi->id);
		return -1;
	}

	return 0;
}

int
ulogd_db_interp_batch(struct ulogd_pluginstance *pi)
{
	struct db_instance *di = upi_priv(pi);
	struct db_row *row;

	pr_fn_debug("pi=%p\n", pi);

	if ((row = db_row_new(pi)) == NULL)
		return ULOGD_IRET_ERR;

	row->ip_saddr = pi->input.keys[0].u.source->u.value.ui32;
	row->ip_daddr = pi->input.keys[1].u.source->u.value.ui32;
	row->ip_proto = pi->input.keys[2].u.source->u.value.ui8;
	row->l4_dport = pi->input.keys[3].u.source->u.value.ui16;
	row->raw_in_pktlen = pi->input.keys[4].u.source->u.value.ui32;
	row->raw_in_pktcount = pi->input.keys[5].u.source->u.value.ui32;
	row->raw_out_pktlen = pi->input.keys[6].u.source->u.value.ui32;
	row->raw_out_pktlen = pi->input.keys[7].u.source->u.value.ui32;
	row->flow_start_day = pi->input.keys[8].u.source->u.value.ui32;
	row->flow_start_sec = pi->input.keys[9].u.source->u.value.ui32;
	row->flow_duration = pi->input.keys[10].u.source->u.value.ui32;

	if (db_row_add(pi, row) < 0)
		return ULOGD_IRET_OK;

	if (di->num_rows >= di->buffer_size)
		__db_commit(pi);

	return ULOGD_IRET_OK;
}