/*
** Zabbix
** Copyright (C) 2001-2017 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "log.h"
#include "dbcache.h"

#define ZBX_DBCONFIG_IMPL
#include "dbconfig.h"
#include "dbsync.h"

extern unsigned char	program_type;

typedef struct
{
	zbx_hashset_t	strpool;
	int		refcount;
}
zbx_dbsync_env_t;

static zbx_dbsync_env_t	dbsync_env;

/* string pool support */

#define REFCOUNT_FIELD_SIZE	sizeof(zbx_uint32_t)

static zbx_hash_t	dbsync_strpool_hash_func(const void *data)
{
	return ZBX_DEFAULT_STRING_HASH_FUNC((char *)data + REFCOUNT_FIELD_SIZE);
}

static int	dbsync_strpool_compare_func(const void *d1, const void *d2)
{
	return strcmp((char *)d1 + REFCOUNT_FIELD_SIZE, (char *)d2 + REFCOUNT_FIELD_SIZE);
}

static char	*dbsync_strdup(const char *str)
{
	void	*ptr;

	ptr = zbx_hashset_search(&dbsync_env.strpool, str - REFCOUNT_FIELD_SIZE);

	if (NULL == ptr)
	{
		ptr = zbx_hashset_insert_ext(&dbsync_env.strpool, str - REFCOUNT_FIELD_SIZE,
				REFCOUNT_FIELD_SIZE + strlen(str) + 1, REFCOUNT_FIELD_SIZE);

		*(zbx_uint32_t *)ptr = 0;
	}

	(*(zbx_uint32_t *)ptr)++;

	return (char *)ptr + REFCOUNT_FIELD_SIZE;
}

static void	dbsync_strfree(char *str)
{
	if (NULL != str)
	{
		void	*ptr = str - REFCOUNT_FIELD_SIZE;

		if (0 == --(*(zbx_uint32_t *)ptr))
			zbx_hashset_remove_direct(&dbsync_env.strpool, ptr);
	}
}

/* zbx_uint64_pair_t hashset support */
static zbx_hash_t	uint64_pair_hash_func(const void *data)
{
	const zbx_uint64_pair_t	*pair = (const zbx_uint64_pair_t *)data;

	zbx_hash_t		hash;

	hash = ZBX_DEFAULT_UINT64_HASH_FUNC(&pair->first);
	hash = ZBX_DEFAULT_UINT64_HASH_ALGO(&pair->second, sizeof(pair->second), hash);

	return hash;
}

static int	uint64_pair_compare_func(const void *d1, const void *d2)
{
	const zbx_uint64_pair_t	*p1 = (const zbx_uint64_pair_t *)d1;
	const zbx_uint64_pair_t	*p2 = (const zbx_uint64_pair_t *)d2;

	ZBX_RETURN_IF_NOT_EQUAL(p1->first, p2->first);
	ZBX_RETURN_IF_NOT_EQUAL(p1->second, p2->second);

	return 0;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_env_init                                                  *
 *                                                                            *
 ******************************************************************************/
static void	dbsync_env_init()
{
	if (0 < dbsync_env.refcount++)
		return;

	zbx_hashset_create(&dbsync_env.strpool, 100, dbsync_strpool_hash_func, dbsync_strpool_compare_func);
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_env_release                                               *
 *                                                                            *
 ******************************************************************************/
static void	dbsync_env_release()
{
	if (0 < --dbsync_env.refcount)
		return;

	zbx_hashset_destroy(&dbsync_env.strpool);
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_uint64                                            *
 *                                                                            *
 * Purpose: compares 64 bit unsigned integer with a raw database value        *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_uint64(const char *value_raw, zbx_uint64_t value)
{
	zbx_uint64_t	value_ui64;

	ZBX_DBROW2UINT64(value_ui64, value_raw);

	return (value_ui64 == value ? SUCCEED : FAIL);
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_int                                               *
 *                                                                            *
 * Purpose: compares 32 bit signed integer with a raw database value          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_int(const char *value_raw, int value)
{
	return (atoi(value_raw) == value ? SUCCEED : FAIL);
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_uchar                                             *
 *                                                                            *
 * Purpose: compares unsigned character with a raw database value             *
 *                                                                            *
 ******************************************************************************/

static int	dbsync_compare_uchar(const char *value_raw, unsigned char value)
{
	unsigned char	value_uchar;

	ZBX_STR2UCHAR(value_uchar, value_raw);
	return (value_uchar == value ? SUCCEED : FAIL);
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_str                                               *
 *                                                                            *
 * Purpose: compares string with a raw database value                         *
 *                                                                            *
 ******************************************************************************/

static int	dbsync_compare_str(const char *value_raw, const char *value)
{
	return (0 == strcmp(value_raw, value) ? SUCCEED : FAIL);
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_add_row                                                   *
 *                                                                            *
 * Purpose: adds a new row to the changeset                                   *
 *                                                                            *
 * Parameter: sync  - [IN] the changeset                                      *
 *            rowid - [IN] the row identifier                                 *
 *            tag   - [IN] the row tag (see ZBX_DBSYNC_ROW_ defines)          *
 *            row   - [IN] the row contents (NULL for ZBX_DBSYNC_ROW_REMOVE)  *
 *                                                                            *
 ******************************************************************************/
static void	dbsync_add_row(zbx_dbsync_t *sync, zbx_uint64_t rowid, unsigned char tag, const DB_ROW dbrow)
{
	int			i;
	zbx_dbsync_row_t	*row;

	row = (zbx_dbsync_row_t *)zbx_malloc(NULL, sizeof(zbx_dbsync_row_t));
	row->rowid = rowid;
	row->tag = tag;

	if (NULL != dbrow)
	{
		row->row = (char **)zbx_malloc(NULL, sizeof(char *) * sync->columns_num);

		for (i = 0; i < sync->columns_num; i++)
			row->row[i] = (SUCCEED == DBis_null(dbrow[i]) ? NULL : dbsync_strdup(dbrow[i]));
	}
	else
		row->row = NULL;

	zbx_vector_ptr_append(&sync->rows, row);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_init                                                  *
 *                                                                            *
 * Purpose: initializes changeset                                             *
 *                                                                            *
 ******************************************************************************/
void	zbx_dbsync_init(zbx_dbsync_t *sync)
{
	dbsync_env_init();

	sync->columns_num = 0;
	zbx_vector_ptr_create(&sync->rows);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_clear                                                 *
 *                                                                            *
 * Purpose: frees resources allocated by changeset                            *
 *                                                                            *
 ******************************************************************************/
void	zbx_dbsync_clear(zbx_dbsync_t *sync)
{
	int			i, j;
	zbx_dbsync_row_t	*row;

	for (i = 0; i < sync->rows.values_num; i++)
	{
		row = (zbx_dbsync_row_t *)sync->rows.values[i];

		if (NULL != row->row)
		{
			for (j = 0; j < sync->columns_num; j++)
				dbsync_strfree(row->row[j]);

			zbx_free(row->row);
		}

		zbx_free(row);
	}

	zbx_vector_ptr_destroy(&sync->rows);

	dbsync_env_release();
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_get_stats                                             *
 *                                                                            *
 * Purpose: calculates changeset statistics                                   *
 *                                                                            *
 * Parameters: sync  - [IN] the changeset                                     *
 *             stats - [OUT] the statistics                                   *
 *                                                                            *
 ******************************************************************************/
void	zbx_dbsync_get_stats(const zbx_dbsync_t *sync, zbx_dbsync_stats_t *stats)
{
	int			i;
	zbx_dbsync_row_t	*row;

	stats->add_num = 0;
	stats->update_num = 0;
	stats->remove_num = 0;

	for (i = 0; i < sync->rows.values_num; i++)
	{
		row = (zbx_dbsync_row_t *)sync->rows.values[i];
		switch (row->tag)
		{
			case ZBX_DBSYNC_ROW_ADD:
				stats->add_num++;
				break;
			case ZBX_DBSYNC_ROW_UPDATE:
				stats->update_num++;
				break;
			case ZBX_DBSYNC_ROW_REMOVE:
				stats->remove_num++;
				break;
		}
	}
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_config_row                                        *
 *                                                                            *
 * Purpose: compares config table row with cached configuration data          *
 *                                                                            *
 * Parameter: config - [IN] the cached configuration                          *
 *            row    - [IN] the table row                                     *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_config_row(ZBX_DC_CONFIG_TABLE *config, const DB_ROW row)
{
	int		i;

	if (FAIL == dbsync_compare_int(row[0], config->refresh_unsupported))
		return FAIL;

	if (FAIL == dbsync_compare_uint64(row[1], config->discovery_groupid))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[2], config->snmptrap_logging))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[26], config->default_inventory_mode))
		return FAIL;

	for (i = 0; TRIGGER_SEVERITY_COUNT > i; i++)
	{
		if (FAIL == dbsync_compare_str(row[3 + i], config->severity_name[i]))
			return FAIL;
	}

	/* read housekeeper configuration */
	if (FAIL == dbsync_compare_int(row[9], config->hk.events_mode))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[10], config->hk.events_trigger))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[11], config->hk.events_internal))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[12], config->hk.events_discovery))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[13], config->hk.events_autoreg))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[14], config->hk.services_mode))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[15], config->hk.services))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[16], config->hk.audit_mode))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[17], config->hk.audit))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[18], config->hk.sessions_mode))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[19], config->hk.sessions))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[20], config->hk.history_mode))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[22], config->hk.history))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[21], config->hk.history_global))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[23], config->hk.trends_mode))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[25], config->hk.trends))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[24], config->hk.trends_global))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_config                                        *
 *                                                                            *
 * Purpose: compares config table with cached configuration data              *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_config(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW		row;
	DB_RESULT	result;
	unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

	sync->columns_num = 27;

	if (NULL == (result = DBselect("select refresh_unsupported,discovery_groupid,snmptrap_logging,"
				"severity_name_0,severity_name_1,severity_name_2,"
				"severity_name_3,severity_name_4,severity_name_5,"
				"hk_events_mode,hk_events_trigger,hk_events_internal,"
				"hk_events_discovery,hk_events_autoreg,hk_services_mode,"
				"hk_services,hk_audit_mode,hk_audit,hk_sessions_mode,hk_sessions,"
				"hk_history_mode,hk_history_global,hk_history,hk_trends_mode,"
				"hk_trends_global,hk_trends,default_inventory_mode"
			" from config")))
	{
		return FAIL;
	}

	if (NULL == (row = DBfetch(result)))
	{
		if (0 != (program_type & ZBX_PROGRAM_TYPE_SERVER))
			zabbix_log(LOG_LEVEL_ERR, "no records in table 'config'");

		goto out;
	}

	if (NULL == cache->config)
		tag = ZBX_DBSYNC_ROW_ADD;
	else if (FAIL == dbsync_compare_config_row(cache->config, row))
		tag = ZBX_DBSYNC_ROW_UPDATE;

	if (ZBX_DBSYNC_ROW_NONE != tag)
		dbsync_add_row(sync, 0, tag, row);

	if (NULL != (row = DBfetch(result)))	/* config table should have only one record */
		zabbix_log(LOG_LEVEL_ERR, "table 'config' has multiple records");

out:
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_host                                              *
 *                                                                            *
 * Purpose: compares hosts table row with cached configuration data           *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            host  - [IN] the cached host                                    *
 *            row   - [IN] the database row                                   *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_host(ZBX_DC_CONFIG *cache, const ZBX_DC_HOST *host, const DB_ROW row)
{
	signed char	ipmi_authtype;
	unsigned char	ipmi_privilege;
	ZBX_DC_IPMIHOST	*ipmihost;

	if (FAIL == dbsync_compare_uint64(row[1], host->proxy_hostid))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[22], host->status))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[2], host->host))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[23], host->name))
		return FAIL;

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	if (FAIL == dbsync_compare_str(row[31], host->tls_issuer))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[32], host->tls_subject))
		return FAIL;

	if ('\0' == *row[33] || '\0' == *row[34])
	{
		if (NULL != host->tls_dc_psk)
			return FAIL;
	}
	else
	{
		if (NULL == host->tls_dc_psk)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[33], host->tls_dc_psk->tls_psk_identity))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[34], host->tls_dc_psk->tls_psk))
			return FAIL;
	}
#endif
	if (FAIL == dbsync_compare_uchar(row[29], host->tls_connect))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[30], host->tls_accept))
		return FAIL;

	/* IPMI hosts */

	ipmi_authtype = (signed char)atoi(row[3]);
	ipmi_privilege = (unsigned char)atoi(row[4]);

	if (0 != ipmi_authtype || 2 != ipmi_privilege || '\0' != *row[5] || '\0' != *row[6])	/* useipmi */
	{
		if (NULL == (ipmihost = (ZBX_DC_IPMIHOST *)zbx_hashset_search(&cache->ipmihosts, &host->hostid)))
			return FAIL;

		if (ipmihost->ipmi_authtype != ipmi_authtype)
			return FAIL;

		if (ipmihost->ipmi_privilege != ipmi_privilege)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[5], ipmihost->ipmi_username))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[6], ipmihost->ipmi_password))
			return FAIL;
	}
	else if (NULL != zbx_hashset_search(&cache->ipmihosts, &host->hostid))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_hosts                                         *
 *                                                                            *
 * Purpose: compares hosts table with cached configuration data               *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_hosts(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_HOST		*host;

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	if (NULL == (result = DBselect(
			"select hostid,proxy_hostid,host,ipmi_authtype,ipmi_privilege,ipmi_username,"
				"ipmi_password,maintenance_status,maintenance_type,maintenance_from,"
				"errors_from,available,disable_until,snmp_errors_from,"
				"snmp_available,snmp_disable_until,ipmi_errors_from,ipmi_available,"
				"ipmi_disable_until,jmx_errors_from,jmx_available,jmx_disable_until,"
				"status,name,lastaccess,error,snmp_error,ipmi_error,jmx_error,tls_connect,tls_accept"
				",tls_issuer,tls_subject,tls_psk_identity,tls_psk"
			" from hosts"
			" where status in (%d,%d,%d,%d)"
				" and flags<>%d",
			HOST_STATUS_MONITORED, HOST_STATUS_NOT_MONITORED,
			HOST_STATUS_PROXY_ACTIVE, HOST_STATUS_PROXY_PASSIVE,
			ZBX_FLAG_DISCOVERY_PROTOTYPE)))
	{
		return FAIL;
	}

	sync->columns_num = 35;
#else
	if (NULL == (result = DBselect(
			"select hostid,proxy_hostid,host,ipmi_authtype,ipmi_privilege,ipmi_username,"
				"ipmi_password,maintenance_status,maintenance_type,maintenance_from,"
				"errors_from,available,disable_until,snmp_errors_from,"
				"snmp_available,snmp_disable_until,ipmi_errors_from,ipmi_available,"
				"ipmi_disable_until,jmx_errors_from,jmx_available,jmx_disable_until,"
				"status,name,lastaccess,error,snmp_error,ipmi_error,jmx_error,tls_connect,tls_accept"
			" from hosts"
			" where status in (%d,%d,%d,%d)"
				" and flags<>%d",
			HOST_STATUS_MONITORED, HOST_STATUS_NOT_MONITORED,
			HOST_STATUS_PROXY_ACTIVE, HOST_STATUS_PROXY_PASSIVE,
			ZBX_FLAG_DISCOVERY_PROTOTYPE)))
	{
		return FAIL;
	}

	sync->columns_num = 31;
#endif

	zbx_hashset_create(&ids, cache->hosts.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (host = (ZBX_DC_HOST *)zbx_hashset_search(&cache->hosts, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_host(cache, host, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);

	}

	zbx_hashset_iter_reset(&cache->hosts, &iter);
	while (NULL != (host = (ZBX_DC_HOST *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &host->hostid))
			dbsync_add_row(sync, host->hostid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_host_inventory                                    *
 *                                                                            *
 * Purpose: compares host inventory table row with cached configuration data  *
 *                                                                            *
 * Parameter: hi  - [IN] the cached host inventory data                       *
 *            row - [IN] the database row                                     *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_host_inventory(const ZBX_DC_HOST_INVENTORY *hi, const DB_ROW row)
{
	return dbsync_compare_uchar(row[1], hi->inventory_mode);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_host_inventory                                *
 *                                                                            *
 * Purpose: compares host_inventory table with cached configuration data      *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_host_inventory(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_HOST_INVENTORY	*hi;

	if (NULL == (result = DBselect(
			"select hostid,inventory_mode"
			" from host_inventory")))
	{
		return FAIL;
	}

	sync->columns_num = 2;

	zbx_hashset_create(&ids, cache->host_inventories.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (hi = (ZBX_DC_HOST_INVENTORY *)zbx_hashset_search(&cache->host_inventories, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_host_inventory(hi, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);

	}

	zbx_hashset_iter_reset(&cache->host_inventories, &iter);
	while (NULL != (hi = (ZBX_DC_HOST_INVENTORY *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &hi->hostid))
			dbsync_add_row(sync, hi->hostid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_host_templates                                *
 *                                                                            *
 * Purpose: compares hosts_templates table with cached configuration data     *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_host_templates(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_iter_t	iter;
	ZBX_DC_HTMPL		*htmpl;
	zbx_hashset_t		htmpls;
	int			i;
	zbx_uint64_pair_t	ht_local, *ht;
	char			hostid_s[MAX_ID_LEN + 1], templateid_s[MAX_ID_LEN + 1];
	char			*del_row[2] = {hostid_s, templateid_s};

	if (NULL == (result = DBselect(
			"select hostid,templateid"
			" from hosts_templates"
			" order by hostid")))
	{
		return FAIL;
	}

	zbx_hashset_create(&htmpls, cache->htmpls.num_data * 5, uint64_pair_hash_func, uint64_pair_compare_func);
	sync->columns_num = 2;

	/* index all host->template links */
	zbx_hashset_iter_reset(&cache->htmpls, &iter);
	while (NULL != (htmpl = (ZBX_DC_HTMPL *)zbx_hashset_iter_next(&iter)))
	{
		ht_local.first = htmpl->hostid;

		for (i = 0; i < htmpl->templateids.values_num; i++)
		{
			ht_local.second = htmpl->templateids.values[i];
			zbx_hashset_insert(&htmpls, &ht_local, sizeof(ht_local));
		}
	}

	/* add new rows, remove existing rows from index */
	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(ht_local.first, row[0]);
		ZBX_STR2UINT64(ht_local.second, row[1]);

		if (NULL == (ht = (zbx_uint64_pair_t *)zbx_hashset_search(&htmpls, &ht_local)))
			dbsync_add_row(sync, 0, ZBX_DBSYNC_ROW_ADD, row);
		else
			zbx_hashset_remove_direct(&htmpls, ht);
	}

	/* add removed rows */
	zbx_hashset_iter_reset(&htmpls, &iter);
	while (NULL != (ht = (zbx_uint64_pair_t *)zbx_hashset_iter_next(&iter)))
	{
		zbx_snprintf(hostid_s, sizeof(hostid_s), ZBX_FS_UI64, ht->first);
		zbx_snprintf(templateid_s, sizeof(templateid_s), ZBX_FS_UI64, ht->second);
		dbsync_add_row(sync, 0, ZBX_DBSYNC_ROW_REMOVE, del_row);
	}

	DBfree_result(result);
	zbx_hashset_destroy(&htmpls);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_global_macro                                      *
 *                                                                            *
 * Purpose: compares global macro table row with cached configuration data    *
 *                                                                            *
 * Parameter: gmacro - [IN] the cached global macro data                      *
 *            row -    [IN] the database row                                  *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_global_macro(const ZBX_DC_GMACRO *gmacro, const DB_ROW row)
{
	char	*macro = NULL, *context = NULL;
	int	ret = FAIL;

	if (FAIL == dbsync_compare_str(row[2], gmacro->value))
		return FAIL;

	if (SUCCEED != zbx_user_macro_parse_dyn(row[1], &macro, &context, NULL))
		return FAIL;

	if (0 != strcmp(gmacro->macro, macro))
		goto out;

	if (NULL == context)
	{
		if (NULL != gmacro->context)
			goto out;

		ret = SUCCEED;
		goto out;
	}

	if (NULL == gmacro->context)
		goto out;

	if (0 == strcmp(gmacro->context, context))
		ret = SUCCEED;
out:
	zbx_free(macro);
	zbx_free(context);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_global_macros                                 *
 *                                                                            *
 * Purpose: compares global macros table with cached configuration data       *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_global_macros(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_GMACRO		*gmacro;

	if (NULL == (result = DBselect(
			"select globalmacroid,macro,value"
			" from globalmacro")))
	{
		return FAIL;
	}

	sync->columns_num = 3;

	zbx_hashset_create(&ids, cache->gmacros.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (gmacro = (ZBX_DC_GMACRO *)zbx_hashset_search(&cache->gmacros, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_global_macro(gmacro, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->gmacros, &iter);
	while (NULL != (gmacro = (ZBX_DC_GMACRO *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &gmacro->globalmacroid))
			dbsync_add_row(sync, gmacro->globalmacroid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_host_macro                                        *
 *                                                                            *
 * Purpose: compares host macro table row with cached configuration data      *
 *                                                                            *
 * Parameter: hmacro - [IN] the cached host macro data                        *
 *            row -    [IN] the database row                                  *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_host_macro(const ZBX_DC_HMACRO *hmacro, const DB_ROW row)
{
	char	*macro = NULL, *context = NULL;
	int	ret = FAIL;

	if (FAIL == dbsync_compare_str(row[3], hmacro->value))
		return FAIL;

	if (FAIL == dbsync_compare_uint64(row[1], hmacro->hostid))
		return FAIL;

	if (SUCCEED != zbx_user_macro_parse_dyn(row[2], &macro, &context, NULL))
		return FAIL;

	if (0 != strcmp(hmacro->macro, macro))
		goto out;

	if (NULL == context)
	{
		if (NULL != hmacro->context)
			goto out;

		ret = SUCCEED;
		goto out;
	}

	if (NULL == hmacro->context)
		goto out;

	if (0 == strcmp(hmacro->context, context))
		ret = SUCCEED;
out:
	zbx_free(macro);
	zbx_free(context);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_host_macros                                   *
 *                                                                            *
 * Purpose: compares global macros table with cached configuration data       *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_host_macros(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_HMACRO		*hmacro;

	if (NULL == (result = DBselect(
			"select hostmacroid,hostid,macro,value"
			" from hostmacro")))
	{
		return FAIL;
	}

	sync->columns_num = 4;

	zbx_hashset_create(&ids, cache->hmacros.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (hmacro = (ZBX_DC_HMACRO *)zbx_hashset_search(&cache->hmacros, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_host_macro(hmacro, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->hmacros, &iter);
	while (NULL != (hmacro = (ZBX_DC_HMACRO *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &hmacro->hostmacroid))
			dbsync_add_row(sync, hmacro->hostmacroid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_interface                                         *
 *                                                                            *
 * Purpose: compares interface table row with cached configuration data       *
 *                                                                            *
 * Parameter: interface - [IN] the cached interface data                      *
 *            row       - [IN] the database row                               *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 * Comments: User macros used in ip, dns fields will always make compare to   *
 *           fail.                                                            *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_interface(const ZBX_DC_INTERFACE *interface, const DB_ROW row)
{
	if (FAIL == dbsync_compare_uint64(row[1], interface->hostid))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[2], interface->type))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[3], interface->main))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[4], interface->useip))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[8], interface->bulk))
		return FAIL;

	if (NULL != strstr(row[5], "{$"))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[5], interface->ip))
		return FAIL;

	if (NULL != strstr(row[6], "{$"))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[6], interface->dns))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[7], interface->port))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_interfaces                                    *
 *                                                                            *
 * Purpose: compares interfaces table with cached configuration data          *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_interfaces(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_INTERFACE	*interface;

	if (NULL == (result = DBselect(
			"select interfaceid,hostid,type,main,useip,ip,dns,port,bulk"
			" from interface")))
	{
		return FAIL;
	}

	sync->columns_num = 9;

	zbx_hashset_create(&ids, cache->interfaces.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (interface = (ZBX_DC_INTERFACE *)zbx_hashset_search(&cache->interfaces, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_interface(interface, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->interfaces, &iter);
	while (NULL != (interface = (ZBX_DC_INTERFACE *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &interface->interfaceid))
			dbsync_add_row(sync, interface->interfaceid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_item                                              *
 *                                                                            *
 * Purpose: compares items table row with cached configuration data           *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            item  - [IN] the cached item                                    *
 *            row   - [IN] the database row                                   *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_item(ZBX_DC_CONFIG *cache, const ZBX_DC_ITEM *item, const DB_ROW row)
{
	ZBX_DC_NUMITEM		*numitem;
	ZBX_DC_SNMPITEM		*snmpitem;
	ZBX_DC_IPMIITEM		*ipmiitem;
	ZBX_DC_FLEXITEM		*flexitem;
	ZBX_DC_TRAPITEM		*trapitem;
	ZBX_DC_LOGITEM		*logitem;
	ZBX_DC_DBITEM		*dbitem;
	ZBX_DC_SSHITEM		*sshitem;
	ZBX_DC_TELNETITEM	*telnetitem;
	ZBX_DC_SIMPLEITEM	*simpleitem;
	ZBX_DC_JMXITEM		*jmxitem;
	ZBX_DC_CALCITEM		*calcitem;
	unsigned char		value_type, type;

	if (FAIL == dbsync_compare_uint64(row[1], item->hostid))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[2], item->status))
		return FAIL;

	ZBX_STR2UCHAR(type, row[3]);
	if (item->type != type)
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[4], item->data_type))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[9], item->port))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[26], item->flags))
		return FAIL;

	if (FAIL == dbsync_compare_uint64(row[27], item->interfaceid))
		return FAIL;

	if (ZBX_HK_OPTION_ENABLED == cache->config->hk.history_global)
	{
		if (item->history != cache->config->hk.history)
			return FAIL;
	}
	else
	{
		if (FAIL == dbsync_compare_int(row[36], item->history))
			return FAIL;
	}

	if (FAIL == dbsync_compare_uchar(row[38], item->inventory_link))
		return FAIL;

	if (FAIL == dbsync_compare_uint64(row[39], item->valuemapid))
		return FAIL;

	ZBX_STR2UCHAR(value_type, row[5]);
	if (item->value_type != value_type)
		return FAIL;

	if (FAIL == dbsync_compare_str(row[6], item->key))
		return FAIL;

	if (FAIL == dbsync_compare_int(row[15], item->delay))
		return FAIL;

	flexitem = (ZBX_DC_FLEXITEM *)zbx_hashset_search(&cache->flexitems, &item->itemid);
	if ('\0' != *row[16])
	{
		if (NULL == flexitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[16], flexitem->delay_flex))
			return FAIL;
	}
	else if (NULL != flexitem)
		return FAIL;

	numitem = (ZBX_DC_NUMITEM *)zbx_hashset_search(&cache->numitems, &item->itemid);
	if (ITEM_VALUE_TYPE_FLOAT == value_type || ITEM_VALUE_TYPE_UINT64 == value_type)
	{
		if (NULL == numitem)
			return FAIL;

		if (FAIL == dbsync_compare_uchar(row[33], numitem->delta))
			return FAIL;

		if (FAIL == dbsync_compare_uchar(row[34], numitem->multiplier))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[35], numitem->formula))
			return FAIL;

		if (ZBX_HK_OPTION_ENABLED == cache->config->hk.trends_global)
		{
			if (numitem->trends != cache->config->hk.trends)
				return FAIL;
		}
		else
		{
			if (FAIL == dbsync_compare_int(row[37], numitem->trends))
				return FAIL;
		}

		if (FAIL == dbsync_compare_str(row[40], numitem->units))
			return FAIL;
	}
	else if (NULL != numitem)
		return FAIL;

	snmpitem = (ZBX_DC_SNMPITEM *)zbx_hashset_search(&cache->snmpitems, &item->itemid);
	if (SUCCEED == is_snmp_type(type))
	{
		if (NULL == snmpitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[7], snmpitem->snmp_community))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[10], snmpitem->snmpv3_securityname))
			return FAIL;

		if (FAIL == dbsync_compare_uchar(row[11], snmpitem->snmpv3_securitylevel))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[12], snmpitem->snmpv3_authpassphrase))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[13], snmpitem->snmpv3_privpassphrase))
			return FAIL;

		if (FAIL == dbsync_compare_uchar(row[28], snmpitem->snmpv3_authprotocol))
			return FAIL;

		if (FAIL == dbsync_compare_uchar(row[29], snmpitem->snmpv3_privprotocol))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[30], snmpitem->snmpv3_contextname))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[8], snmpitem->snmp_oid))
			return FAIL;
	}
	else if (NULL != snmpitem)
		return FAIL;

	ipmiitem = (ZBX_DC_IPMIITEM *)zbx_hashset_search(&cache->ipmiitems, &item->itemid);
	if (ITEM_TYPE_IPMI == item->type)
	{
		if (NULL == ipmiitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[14], ipmiitem->ipmi_sensor))
			return FAIL;
	}
	else if (NULL != ipmiitem)
		return FAIL;

	trapitem = (ZBX_DC_TRAPITEM *)zbx_hashset_search(&cache->trapitems, &item->itemid);
	if (ITEM_TYPE_TRAPPER == item->type && '\0' != *row[17])
	{
		if (NULL == trapitem)
			return FAIL;

		zbx_trim_str_list(row[17], ',');

		if (FAIL == dbsync_compare_str(row[17], trapitem->trapper_hosts))
			return FAIL;
	}
	else if (NULL != trapitem)
		return FAIL;

	logitem = (ZBX_DC_LOGITEM *)zbx_hashset_search(&cache->logitems, &item->itemid);
	if (ITEM_VALUE_TYPE_LOG == item->value_type && '\0' != *row[18])
	{
		if (NULL == logitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[18], logitem->logtimefmt))
			return FAIL;
	}
	else if (NULL != logitem)
		return FAIL;

	dbitem = (ZBX_DC_DBITEM *)zbx_hashset_search(&cache->dbitems, &item->itemid);
	if (ITEM_TYPE_DB_MONITOR == item->type && '\0' != *row[19])
	{
		if (NULL == dbitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[19], dbitem->params))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[22], dbitem->username))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[23], dbitem->password))
			return FAIL;
	}
	else if (NULL != dbitem)
		return FAIL;

	sshitem = (ZBX_DC_SSHITEM *)zbx_hashset_search(&cache->sshitems, &item->itemid);
	if (ITEM_TYPE_SSH == item->type)
	{
		if (NULL == sshitem)
			return FAIL;

		if (FAIL == dbsync_compare_uchar(row[21], sshitem->authtype))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[22], sshitem->username))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[23], sshitem->password))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[24], sshitem->publickey))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[25], sshitem->privatekey))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[19], sshitem->params))
			return FAIL;
	}
	else if (NULL != sshitem)
		return FAIL;

	telnetitem = (ZBX_DC_TELNETITEM *)zbx_hashset_search(&cache->telnetitems, &item->itemid);
	if (ITEM_TYPE_TELNET == item->type)
	{
		if (NULL == telnetitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[22], telnetitem->username))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[23], telnetitem->password))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[19], telnetitem->params))
			return FAIL;
	}
	else if (NULL != telnetitem)
		return FAIL;

	simpleitem = (ZBX_DC_SIMPLEITEM *)zbx_hashset_search(&cache->simpleitems, &item->itemid);
	if (ITEM_TYPE_SIMPLE == item->type)
	{
		if (NULL == simpleitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[22], simpleitem->username))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[23], simpleitem->password))
			return FAIL;
	}
	else if (NULL != simpleitem)
		return FAIL;

	jmxitem = (ZBX_DC_JMXITEM *)zbx_hashset_search(&cache->jmxitems, &item->itemid);
	if (ITEM_TYPE_JMX == item->type)
	{
		if (NULL == jmxitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[22], jmxitem->username))
			return FAIL;

		if (FAIL == dbsync_compare_str(row[23], jmxitem->password))
			return FAIL;
	}
	else if (NULL != jmxitem)
		return FAIL;

	calcitem = (ZBX_DC_CALCITEM *)zbx_hashset_search(&cache->calcitems, &item->itemid);
	if (ITEM_TYPE_CALCULATED == item->type)
	{
		if (NULL == calcitem)
			return FAIL;

		if (FAIL == dbsync_compare_str(row[19], calcitem->params))
			return FAIL;
	}
	else if (NULL != calcitem)
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_items                                         *
 *                                                                            *
 * Purpose: compares items table with cached configuration data               *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_items(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_ITEM		*item;

	if (NULL == (result = DBselect(
			"select i.itemid,i.hostid,i.status,i.type,i.data_type,i.value_type,i.key_,"
				"i.snmp_community,i.snmp_oid,i.port,i.snmpv3_securityname,i.snmpv3_securitylevel,"
				"i.snmpv3_authpassphrase,i.snmpv3_privpassphrase,i.ipmi_sensor,i.delay,i.delay_flex,"
				"i.trapper_hosts,i.logtimefmt,i.params,i.state,i.authtype,i.username,i.password,"
				"i.publickey,i.privatekey,i.flags,i.interfaceid,i.snmpv3_authprotocol,"
				"i.snmpv3_privprotocol,i.snmpv3_contextname,i.lastlogsize,i.mtime,i.delta,i.multiplier,"
				"i.formula,i.history,i.trends,i.inventory_link,i.valuemapid,i.units,i.error"
			" from items i,hosts h"
			" where i.hostid=h.hostid"
				" and h.status in (%d,%d)"
				" and i.flags<>%d",
			HOST_STATUS_MONITORED, HOST_STATUS_NOT_MONITORED,
			ZBX_FLAG_DISCOVERY_PROTOTYPE)))
	{
		return FAIL;
	}

	sync->columns_num = 42;

	zbx_hashset_create(&ids, cache->items.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (item = (ZBX_DC_ITEM *)zbx_hashset_search(&cache->items, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_item(cache, item, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->items, &iter);
	while (NULL != (item = (ZBX_DC_ITEM *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &item->itemid))
			dbsync_add_row(sync, item->itemid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_trigger                                           *
 *                                                                            *
 * Purpose: compares triggers table row with cached configuration data        *
 *                                                                            *
 * Parameter: trigger - [IN] the cached trigger                               *
 *            row     - [IN] the database row                                 *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_trigger(const ZBX_DC_TRIGGER *trigger, const DB_ROW row)
{
	if (FAIL == dbsync_compare_str(row[1], trigger->description))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[2], trigger->expression))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[4], trigger->priority))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[5], trigger->type))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[9], trigger->status))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_triggers                                      *
 *                                                                            *
 * Purpose: compares triggers table with cached configuration data            *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_triggers(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_TRIGGER		*trigger;

	if (NULL == (result = DBselect(
			"select distinct t.triggerid,t.description,t.expression,t.error,"
				"t.priority,t.type,t.value,t.state,t.lastchange,t.status"
			" from hosts h,items i,functions f,triggers t"
			" where h.hostid=i.hostid"
				" and i.itemid=f.itemid"
				" and f.triggerid=t.triggerid"
				" and h.status in (%d,%d)"
				" and t.flags<>%d",
			HOST_STATUS_MONITORED, HOST_STATUS_NOT_MONITORED,
			ZBX_FLAG_DISCOVERY_PROTOTYPE)))
	{
		return FAIL;
	}

	sync->columns_num = 10;

	zbx_hashset_create(&ids, cache->triggers.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (trigger = (ZBX_DC_TRIGGER *)zbx_hashset_search(&cache->triggers, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_trigger(trigger, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->triggers, &iter);
	while (NULL != (trigger = (ZBX_DC_TRIGGER *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &trigger->triggerid))
			dbsync_add_row(sync, trigger->triggerid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_trigger_dependency                            *
 *                                                                            *
 * Purpose: compares trigger_depends table with cached configuration data     *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_trigger_dependency(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		deps;
	zbx_hashset_iter_t	iter;
	ZBX_DC_TRIGGER_DEPLIST	*dep_down, *dep_up;
	zbx_uint64_pair_t	*dep, dep_local;
	char			down_s[MAX_ID_LEN + 1], up_s[MAX_ID_LEN + 1];
	char			*del_row[2] = {down_s, up_s};
	int			i;

	if (NULL == (result = DBselect(
			"select d.triggerid_down,d.triggerid_up"
			" from trigger_depends d,hosts h,items i,functions f"
			" where h.hostid=i.hostid"
				" and i.itemid=f.itemid"
				" and f.triggerid=d.triggerid_down"
				" and h.status in (%d,%d)",
			HOST_STATUS_MONITORED, HOST_STATUS_NOT_MONITORED)))
	{
		return FAIL;
	}

	sync->columns_num = 2;

	zbx_hashset_create(&deps, cache->trigdeps.num_data * 5, uint64_pair_hash_func, uint64_pair_compare_func);

	/* index all host->template links */
	zbx_hashset_iter_reset(&cache->trigdeps, &iter);
	while (NULL != (dep_down = (ZBX_DC_TRIGGER_DEPLIST *)zbx_hashset_iter_next(&iter)))
	{
		dep_local.first = dep_down->triggerid;

		for (i = 0; i < dep_down->dependencies.values_num; i++)
		{
			dep_up = (ZBX_DC_TRIGGER_DEPLIST *)dep_down->dependencies.values[i];
			dep_local.second = dep_up->triggerid;
			zbx_hashset_insert(&deps, &dep_local, sizeof(dep_local));
		}
	}

	/* add new rows, remove existing rows from index */
	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(dep_local.first, row[0]);
		ZBX_STR2UINT64(dep_local.second, row[1]);

		if (NULL == (dep = (zbx_uint64_pair_t *)zbx_hashset_search(&deps, &dep_local)))
			dbsync_add_row(sync, 0, ZBX_DBSYNC_ROW_ADD, row);
		else
			zbx_hashset_remove_direct(&deps, dep);
	}

	/* add removed rows */
	zbx_hashset_iter_reset(&deps, &iter);
	while (NULL != (dep = (zbx_uint64_pair_t *)zbx_hashset_iter_next(&iter)))
	{
		zbx_snprintf(down_s, sizeof(down_s), ZBX_FS_UI64, dep->first);
		zbx_snprintf(up_s, sizeof(up_s), ZBX_FS_UI64, dep->second);
		dbsync_add_row(sync, 0, ZBX_DBSYNC_ROW_REMOVE, del_row);
	}

	DBfree_result(result);
	zbx_hashset_destroy(&deps);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_function                                          *
 *                                                                            *
 * Purpose: compares functions table row with cached configuration data       *
 *                                                                            *
 * Parameter: function - [IN] the cached function                             *
 *            row      - [IN] the database row                                *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_function(const ZBX_DC_FUNCTION *function, const DB_ROW row)
{
	if (FAIL == dbsync_compare_uint64(row[0], function->itemid))
		return FAIL;

	if (FAIL == dbsync_compare_uint64(row[4], function->triggerid))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[2], function->function))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[3], function->parameter))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_functions                                     *
 *                                                                            *
 * Purpose: compares functions table with cached configuration data           *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_functions(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_FUNCTION		*function;

	if (NULL == (result = DBselect(
			"select i.itemid,f.functionid,f.function,f.parameter,t.triggerid"
			" from hosts h,items i,functions f,triggers t"
			" where h.hostid=i.hostid"
				" and i.itemid=f.itemid"
				" and f.triggerid=t.triggerid"
				" and h.status in (%d,%d)"
				" and t.flags<>%d",
			HOST_STATUS_MONITORED, HOST_STATUS_NOT_MONITORED,
			ZBX_FLAG_DISCOVERY_PROTOTYPE)))
	{
		return FAIL;
	}

	sync->columns_num = 5;

	zbx_hashset_create(&ids, cache->functions.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[1]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (function = (ZBX_DC_FUNCTION *)zbx_hashset_search(&cache->functions, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_function(function, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->functions, &iter);
	while (NULL != (function = (ZBX_DC_FUNCTION *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &function->functionid))
			dbsync_add_row(sync, function->functionid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_expression                                        *
 *                                                                            *
 * Purpose: compares expressions table row with cached configuration data     *
 *                                                                            *
 * Parameter: expression - [IN] the cached expression                         *
 *            row        - [IN] the database row                              *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_expression(const ZBX_DC_EXPRESSION *expression, const DB_ROW row)
{
	if (FAIL == dbsync_compare_str(row[0], expression->regexp))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[2], expression->expression))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[3], expression->type))
		return FAIL;

	if (*row[4] != expression->delimiter)
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[5], expression->case_sensitive))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_exprssions                                    *
 *                                                                            *
 * Purpose: compares expressions, regexps tables with cached configuration    *
 *          data                                                              *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_expressions(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	ZBX_DC_EXPRESSION	*expression;

	if (NULL == (result = DBselect(
			"select r.name,e.expressionid,e.expression,e.expression_type,e.exp_delimiter,e.case_sensitive"
			" from regexps r,expressions e"
			" where r.regexpid=e.regexpid")))
	{
		return FAIL;
	}

	sync->columns_num = 6;

	zbx_hashset_create(&ids, cache->expressions.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[1]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (expression = (ZBX_DC_EXPRESSION *)zbx_hashset_search(&cache->expressions, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_expression(expression, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->expressions, &iter);
	while (NULL != (expression = (ZBX_DC_EXPRESSION *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &expression->expressionid))
			dbsync_add_row(sync, expression->expressionid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_action                                            *
 *                                                                            *
 * Purpose: compares actions table row with cached configuration data         *
 *                                                                            *
 * Parameter: action - [IN] the cached action                                 *
 *            row    - [IN] the database row                                  *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_action(const zbx_dc_action_t *action, const DB_ROW row)
{

	if (FAIL == dbsync_compare_uchar(row[1], action->eventsource))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[2], action->evaltype))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[3], action->formula))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_actions                                       *
 *                                                                            *
 * Purpose: compares actions table with cached configuration data             *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_actions(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW			row;
	DB_RESULT		result;
	zbx_hashset_t		ids;
	zbx_hashset_iter_t	iter;
	zbx_uint64_t		rowid;
	zbx_dc_action_t		*action;

	if (NULL == (result = DBselect(
			"select actionid,eventsource,evaltype,formula"
			" from actions"
			" where status=%d",
			ACTION_STATUS_ACTIVE)))
	{
		return FAIL;
	}

	sync->columns_num = 4;

	zbx_hashset_create(&ids, cache->actions.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (action = (zbx_dc_action_t *)zbx_hashset_search(&cache->actions, &rowid)))
			tag = ZBX_DBSYNC_ROW_ADD;
		else if (FAIL == dbsync_compare_action(action, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->actions, &iter);
	while (NULL != (action = (zbx_dc_action_t *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &action->actionid))
			dbsync_add_row(sync, action->actionid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dbsync_compare_action_condition                                  *
 *                                                                            *
 * Purpose: compares conditions table row with cached configuration data      *
 *                                                                            *
 * Parameter: action - [IN] the cached action                                 *
 *            row    - [IN] the database row                                  *
 *                                                                            *
 * Return value: SUCCEED - the row matches configuration data                 *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dbsync_compare_action_condition(const zbx_dc_action_condition_t *condition, const DB_ROW row)
{
	if (FAIL == dbsync_compare_uchar(row[2], condition->conditiontype))
		return FAIL;

	if (FAIL == dbsync_compare_uchar(row[3], condition->operator))
		return FAIL;

	if (FAIL == dbsync_compare_str(row[4], condition->value))
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dbsync_compare_action_conditions                             *
 *                                                                            *
 * Purpose: compares conditions table with cached configuration data          *
 *                                                                            *
 * Parameter: cache - [IN] the configuration cache                            *
 *            sync  - [OUT] the changeset                                     *
 *                                                                            *
 * Return value: SUCCEED - the changeset was successfully calculated          *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dbsync_compare_action_conditions(ZBX_DC_CONFIG *cache, zbx_dbsync_t *sync)
{
	DB_ROW				row;
	DB_RESULT			result;
	zbx_hashset_t			ids;
	zbx_hashset_iter_t		iter;
	zbx_uint64_t			rowid;
	zbx_dc_action_condition_t	*condition;

	if (NULL == (result = DBselect(
			"select c.conditionid,c.actionid,c.conditiontype,c.operator,c.value"
			" from conditions c,actions a"
			" where c.actionid=a.actionid"
				" and a.status=%d",
			ACTION_STATUS_ACTIVE)))
	{
		return FAIL;
	}

	sync->columns_num = 5;

	zbx_hashset_create(&ids, cache->action_conditions.num_data, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	while (NULL != (row = DBfetch(result)))
	{
		unsigned char	tag = ZBX_DBSYNC_ROW_NONE;

		ZBX_STR2UINT64(rowid, row[0]);
		zbx_hashset_insert(&ids, &rowid, sizeof(rowid));

		if (NULL == (condition = (zbx_dc_action_condition_t *)zbx_hashset_search(&cache->action_conditions,
				&rowid)))
		{
			tag = ZBX_DBSYNC_ROW_ADD;
		}
		else if (FAIL == dbsync_compare_action_condition(condition, row))
			tag = ZBX_DBSYNC_ROW_UPDATE;

		if (ZBX_DBSYNC_ROW_NONE != tag)
			dbsync_add_row(sync, rowid, tag, row);
	}

	zbx_hashset_iter_reset(&cache->action_conditions, &iter);
	while (NULL != (condition = (zbx_dc_action_condition_t *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == zbx_hashset_search(&ids, &condition->conditionid))
			dbsync_add_row(sync, condition->conditionid, ZBX_DBSYNC_ROW_REMOVE, NULL);
	}

	zbx_hashset_destroy(&ids);
	DBfree_result(result);

	return SUCCEED;
}

