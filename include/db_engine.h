#ifndef _DB_ENGINE_H_
#define _DB_ENGINE_H_

#include <stdio.h>
#include <pthread.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DB_ENGINE_GID_SIZE
#define DB_ENGINE_GID_SIZE	(128)		// db.h: DB_GID_SIZE
#endif
struct db_handle;
struct db_engine;
typedef struct db_engine_txn
{
	void * priv;
	struct db_engine * engine;
	
// public methods:
	int (* begin)(struct db_engine_txn * txn, struct db_engine_txn * parent_txn);
	int (* commit)(struct db_engine_txn * txn, int flags);
	int (* abort)(struct db_engine_txn * txn);
	
	int (* prepare)(struct db_engine_txn * txn, unsigned char gid[/* DB_ENGINE_GID_SIZE */]);
	int (* discard)(struct db_engine_txn * txn);
	
	int (* set_name)(struct db_engine_txn * txn, const char * name);
	const char * (* get_name)(struct db_engine_txn * txn);
}db_engine_txn_t;
db_engine_txn_t * db_engine_txn_init(db_engine_txn_t * txn, struct db_engine * env);
void db_engine_txn_cleanup(db_engine_txn_t * txn);


typedef struct db_record_data
{
	void * data;
	ssize_t size;
	int flags;
}db_record_data_t;

typedef ssize_t (* db_associate_callback)(struct db_handle * sdb, 
		const db_record_data_t * key, const db_record_data_t * value, // records in the primary db
		db_record_data_t ** p_skeys // return key(s) in the secondary db
	);
typedef struct db_handle
{
	void * priv;
	struct db_engine * engine;
	
	void * user_data;
	unsigned int err_code;
	
	int (* open)(struct db_handle * db, 
		db_engine_txn_t * txn, // nullable 
		const char * name, 
		int db_type, // 0: DB_BTREE, 1: DB_HASH
		int flags);
	int (* associate)(struct db_handle * primary, 
		db_engine_txn_t * txn, // nullable
		struct db_handle * secondary, 
		db_associate_callback associated_by);
	int (* close)(struct db_handle * db);
	
	/**
	 * find(): 				Find value(s) in the primary_db or secondary_db by key 
	 * find_secondary(): 	Use the secondary_key to find the primary_keys and values in the primary_db
	 *   @return num_records, can be greater than 1 if duplicated key is allowed
	 */
	ssize_t (* find)(struct db_handle * db, 
		const db_record_data_t * key, 
		db_record_data_t ** p_values);

	ssize_t (* find_secondary)(struct db_handle * secondary_db, 
		const db_record_data_t * skey,		// the key of secondary database
		db_record_data_t ** p_keys,			// if need return the key of the primary database
		db_record_data_t ** p_values);
	
	int (* insert)(struct db_handle * db, const db_record_data_t * key, const db_record_data_t * value);
	int (* update)(struct db_handle * db, const db_record_data_t * key, const db_record_data_t * value);
	int (* del)(struct db_handle * db, const db_record_data_t * key);

}db_handle_t;
db_handle_t * db_handle_init(db_handle_t * db, struct db_engine * engine, void * user_data);
void db_handle_cleanup(db_handle_t * db);

typedef struct db_engine
{
	void * priv;
	void * user_data;
	
	long refs_count;
	unsigned int err_code;
	
	int (* set_home)(struct db_engine * engine, const char * home_dir);
	
	db_handle_t * (* open_db)(struct db_engine * engine, const char * db_name, int db_type, int flags);
	int (* close_db)(db_handle_t * db);
	
	db_engine_txn_t * (* txn_new)(struct db_engine * engine, struct db_engine_txn * parent_txn);
	void (* txn_free)(struct db_engine * engine, db_engine_txn_t * txn);
}db_engine_t;
db_engine_t * db_engine_init(const char * home_dir, void * user_data);
void db_engine_cleanup(db_engine_t * engine);
db_engine_t * db_engine_get();


#ifdef __cplusplus
}
#endif

#endif