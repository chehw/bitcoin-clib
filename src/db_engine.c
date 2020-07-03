/*
 * db_engine.c
 * 
 * Copyright 2020 Che Hongwei <htc.chehw@gmail.com>
 * 
 * The MIT License (MIT)
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy 
 * of this software and associated documentation files (the "Software"), to deal 
 * in the Software without restriction, including without limitation the rights 
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell 
 * copies of the Software, and to permit persons to whom the Software is 
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all 
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
 * IN THE SOFTWARE.
 * 
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <db.h>
#include <pthread.h>

#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#include "db_engine.h"

static inline DB_ENV * db_engine_get_env(db_engine_t * engine) { return *(DB_ENV **)engine->priv; }

#define db_check_error(ret_code, fmt, ...) do {				\
		if(ret_code) {										\
			db_engine_t * engine = db_engine_get();			\
			DB_ENV * env = db_engine_get_env(engine);		\
			assert(env);									\
			env->err(env, ret_code, fmt, ##__VA_ARGS__);	\
		}													\
	}while(0)

typedef struct db_engine_private
{
	DB_ENV * env;
	pthread_mutex_t mutex;
	db_engine_t * engine;

	char home_dir[PATH_MAX];
	u_int32_t env_flags;
	
	ssize_t max_size;
	ssize_t count;
	db_handle_t ** databases;
	
	db_handle_t * log_db;
	char error_desc[4096];	//  last error description
	
	long refs_count;
}db_engine_private_t;




/**************************************************
 * struct db_engine_txn
 **************************************************/
static inline DB_TXN * db_txn_get_handle(struct db_engine_txn * txn)
{
	if(NULL == txn) return NULL;
	return txn->priv;
}

static int txn_begin(struct db_engine_txn * txn, struct db_engine_txn * parent_txn)
{
	assert(txn && txn->engine);
	assert(NULL == txn->priv);
	
	DB_ENV * env = db_engine_get_env(txn->engine);
	assert(env);
	
	DB_TXN * parent = parent_txn?parent_txn->priv:NULL;
	int rc = env->txn_begin(env, parent, (DB_TXN **)&txn->priv, DB_READ_COMMITTED | DB_TXN_SYNC);
	if(rc) {
		env->err(env, rc, "%s() failed.", __FUNCTION__);
	}
	return rc;
}

static int txn_commit(struct db_engine_txn * txn, int flags)
{
	int rc = -1;
	DB_TXN * db_txn = txn->priv;
	if(db_txn) {
		rc = db_txn->commit(db_txn, flags);
		txn->priv = NULL;
		db_check_error(rc, "%s() failed.", __FUNCTION__);
	}
	return rc;
}
static int txn_abort(struct db_engine_txn * txn)
{
	int rc = -1;
	DB_TXN * db_txn = txn->priv;
	if(db_txn) {
		rc = db_txn->abort(db_txn);
		txn->priv = NULL;
		db_check_error(rc, "%s() failed.", __FUNCTION__);
	}
	return rc;
}

static int txn_prepare(struct db_engine_txn * txn, unsigned char gid[])
{
	int rc = -1;
	DB_TXN * db_txn = txn->priv;
	if(db_txn) {
		rc = db_txn->prepare(db_txn, gid);
		db_check_error(rc, "%s() failed.", __FUNCTION__);
	}
	return rc;
}

static int txn_discard(struct db_engine_txn * txn)
{
	int rc = -1;
	DB_TXN * db_txn = txn->priv;
	if(db_txn) {
		rc = db_txn->discard(db_txn, 0);
		txn->priv = NULL;
		db_check_error(rc, "%s() failed.", __FUNCTION__);
	}
	return rc;
}

static int txn_set_name(struct db_engine_txn * txn, const char * name)
{
	int rc = -1;
	DB_TXN * db_txn = txn->priv;
	if(db_txn) {
		rc = db_txn->set_name(db_txn, name);
		db_check_error(rc, "%s() failed.", __FUNCTION__);
	}
	return rc;
}
static const char * txn_get_name(struct db_engine_txn * txn)
{
	int rc = -1;
	const char * name = NULL;
	DB_TXN * db_txn = txn->priv;
	if(db_txn) {
		rc = db_txn->get_name(db_txn, &name);
		db_check_error(rc, "%s() failed.", __FUNCTION__);
	}
	return name;
}

db_engine_txn_t * db_engine_txn_init(db_engine_txn_t * txn, struct db_engine * engine)
{
	if(NULL == txn) txn = calloc(1, sizeof(*txn));
	txn->engine = engine;
	
	txn->begin = txn_begin;
	txn->commit = txn_commit;
	txn->abort = txn_abort;
	txn->prepare = txn_prepare;
	txn->discard = txn_discard;
	txn->set_name = txn_set_name;
	txn->get_name = txn_get_name;
	
	return txn;
}
void db_engine_txn_cleanup(db_engine_txn_t * txn)
{
	DB_TXN * db_txn = txn->priv;
	if(db_txn) {
		db_txn->discard(db_txn, 0);
	}
	txn->priv = NULL;
	return;
}

/****************************************************************
 * struct db_handle
****************************************************************/
typedef struct db_private
{
	DB * dbp;
	struct db_handle * db;
	int db_type;
	
	pthread_mutex_t mutex;
	char name[PATH_MAX];
	
	db_associate_callback associate_func;
}db_private_t;
static inline DB * db_get_handle(struct db_handle * db) 
{ 
	assert(db && db->priv);
	DB * dbp = ((db_private_t *)db->priv)->dbp;
	assert(dbp);
	return dbp;
}

#define db_lock(db)			pthread_mutex_lock(&((db_private_t *)db->priv)->mutex)
#define db_unlock(db)		pthread_mutex_unlock(&((db_private_t *)db->`priv)->mutex)

db_private_t * db_private_new(db_handle_t * db)
{
	assert(db);
	DB * dbp = NULL;
	DB_ENV * env = db_engine_get_env(db->engine);
	int rc = db_create(&dbp, env, 0);
	if(rc || NULL == dbp ) {
		env->err(env, rc, "%s()::db_create() failed.\n", __FUNCTION__);
		return NULL;
	}
	
	db_private_t * priv = calloc(1, sizeof(*priv));
	assert(priv);
	priv->db = db;
	priv->dbp = dbp;
	rc = pthread_mutex_init(&priv->mutex, NULL);
	assert(0 == rc);
	
	db->priv = priv;
	return priv;
}

void db_private_free(db_private_t * priv)
{
	if(NULL == priv) return;
	
	if(priv->dbp) {
		priv->dbp->close(priv->dbp, 0);
	}
	
	pthread_mutex_destroy(&priv->mutex);
	free(priv);
	return;
}

static int db_open(struct db_handle * db, db_engine_txn_t * txn, const char * name, int db_type, int flags)
{
	assert(db && db->priv && name);
	int rc = -1;
	db_private_t * priv = db->priv;
	DB * dbp = priv->dbp;
	assert(dbp);
	
	
	switch(db_type)
	{
	case 0: priv->db_type = DB_BTREE; break;
	case 1: priv->db_type = DB_HASH; break;
	default:
		priv->db_type = DB_UNKNOWN;
	}
	
	if(flags <= 0) flags = DB_CREATE | DB_AUTO_COMMIT;
	rc = dbp->open(dbp, db_txn_get_handle(txn), name, NULL, priv->db_type, flags, 0660);
	db_check_error(rc, "%s() failed.\n", __FUNCTION__);
	return rc;
}

static inline db_handle_t * db_engine_find_db(db_engine_t * engine, DB * dbp)
{
	assert(engine && engine->priv && dbp);
	db_engine_private_t * priv = engine->priv;
	
	for(int i = 0; i < priv->count; ++i)
	{
		if(dbp == db_get_handle(priv->databases[i])) return priv->databases[i];
	}
	return NULL;
}


static int secondary_db_get_key(DB * secondary, 
	const DBT * key, const DBT * value, 
	DBT * result)
{
	db_engine_t * engine = db_engine_get();
	assert(engine);
	
	db_handle_t * db = db_engine_find_db(engine, secondary);
	assert(db && db->priv);
	
	db_private_t * priv = db->priv;
	assert(priv->associate_func);
	
	db_record_data_t * skeys = NULL;
	
	ssize_t num_keys = priv->associate_func(db, 
		&(db_record_data_t){ .data = key->data, .size = key->size}, 
		&(db_record_data_t){ .data = value->data, .size = value->size},
		&skeys);
		
	if(num_keys < 1 || NULL == skeys) return -1;
	
	if(num_keys == 1)
	{
		result->data = skeys[0].data;
		result->size = skeys[0].size;
	}else
	{
		/**
		 * Working with Multiple Keys 
		 * (if need to be sorted by multiple skeys in the secondary_db)
		 */
		 
		DBT * multiple_keys = calloc(num_keys, sizeof(*multiple_keys));
		assert(multiple_keys);
	
		for(ssize_t i = 0; i < num_keys; ++i)
		{
			multiple_keys[i].data = skeys[i].data;
			multiple_keys[i].size = (u_int32_t)skeys[i].size;
		}

		/* 
		 * set flags for the returned DBT. 
		 * DB_DBT_MULTIPLE is required in order for DB to know that 
		 * the DBT references an array. 
		 * DB_DBT_APPMALLOC is also required since we
		 * dynamically allocated memory for the DBT's data field. 
		 */
		result->flags = DB_DBT_MULTIPLE | DB_DBT_APPMALLOC;
	
		result->data = multiple_keys;
		result->size = num_keys;
	}
	
	free(skeys);
	return 0;
}


static int db_associate(struct db_handle * primary, db_engine_txn_t * txn, 
	struct db_handle * secondary, db_associate_callback associated_by)
{
	db_private_t * priv = secondary->priv;
	priv->associate_func = associated_by;
	
	DB * dbp = db_get_handle(primary);
	DB * sdbp = db_get_handle(secondary);
	int rc = dbp->associate(dbp, db_txn_get_handle(txn), sdbp, secondary_db_get_key, DB_CREATE);
	
	return rc;
}
static int db_close(struct db_handle * db)
{
	return 0;
}
static ssize_t db_find(struct db_handle * db, db_engine_txn_t * _txn, const db_record_data_t * key, db_record_data_t ** p_values)
{
	return 0;
}

static ssize_t db_find_secondary(struct db_handle * secondary_db, db_engine_txn_t * _txn, 
	const db_record_data_t * skey,		// the key of secondary database
	db_record_data_t ** p_keys,			// if need return the key of the primary database
	db_record_data_t ** p_values)
{
	return 0;
}
	

static int db_insert(struct db_handle * db, db_engine_txn_t * _txn, 
	const db_record_data_t * _key, 
	const db_record_data_t * _value)
{
	DBT key, value;
	memset(&key, 0, sizeof(key));
	memset(&value, 0, sizeof(value));
	
	key.data = (void *)_key->data;
	key.size = _key->size;
	
	value.data = (void *)_value->data;
	value.size = _value->size;
	
	DB * dbp = db_get_handle(db);
	assert(dbp);
	DB_TXN * txn = db_txn_get_handle(_txn);
	
	u_int32_t flags = 0;
	if(db->flags & db_record_flags_no_dup) flags |= DB_NODUPDATA;
	if(db->flags & db_record_flags_no_overwrite) flags |= DB_NOOVERWRITE;
	
	int rc = dbp->put(dbp, txn, &key, &value, flags);
	db_check_error(rc, "%s() failed.\n", __FUNCTION__);
	return rc;
}

static int db_update(struct db_handle * db, db_engine_txn_t * _txn, const db_record_data_t * key, const db_record_data_t * value)
{
	return 0;
}

static int db_del(struct db_handle * db, db_engine_txn_t * _txn, const db_record_data_t * key)
{
	return 0;
}


db_handle_t * db_handle_init(db_handle_t * db, db_engine_t * engine, void * user_data)
{
	if(NULL == db) db = calloc(1, sizeof(*db));
	assert(db);
	db->user_data = user_data;
	db->engine = engine;
	
	db->open = db_open;
	db->associate = db_associate;
	db->close = db_close;
	db->find = db_find;
	db->find_secondary = db_find_secondary;
	db->insert = db_insert;
	db->update = db_update;
	db->del = db_del;
	
	db_private_t * priv = db_private_new(db);
	assert(priv && db->priv == priv);
	
	return db;
}
void db_handle_cleanup(db_handle_t * db)
{
	if(NULL == db) return;
	db_private_free(db->priv);
	return;
}


/*****************************************************************
 * struct db_engine
****************************************************************/


#define DB_ENGINE_ALLOC_SIZE	(64)
static int db_engine_resize(db_engine_t * engine, int new_size)
{
	assert(engine && engine->priv);
	db_engine_private_t * priv = engine->priv;
	
	if(new_size <= 0) new_size = DB_ENGINE_ALLOC_SIZE;
	else new_size = (new_size + DB_ENGINE_ALLOC_SIZE - 1) / DB_ENGINE_ALLOC_SIZE * DB_ENGINE_ALLOC_SIZE;
	
	if(new_size < priv->max_size) return 0;
	
	db_handle_t ** dbs = realloc(priv->databases, new_size * sizeof(*dbs));
	assert(dbs);
	
	memset(dbs, 0, (new_size - priv->max_size) * sizeof(*dbs));
	priv->databases	 = dbs;
	priv->max_size = new_size;
	return 0;
}

static db_engine_private_t * db_engine_private_new(db_engine_t * engine)
{
	db_engine_private_t * priv = calloc(1, sizeof(*priv));
	assert(priv);
	
	int rc = db_env_create(&priv->env, 0);
	assert(0 == rc);
	
	// set default evn_flags
	priv->env_flags = DB_CREATE 
		| DB_INIT_MPOOL 
		| DB_INIT_LOG
		| DB_INIT_LOCK
		| DB_INIT_TXN
		| DB_RECOVER
		| DB_REGISTER
		| DB_THREAD
		| 0;
	
	rc = pthread_mutex_init(&priv->mutex, NULL);
	assert(0 == rc);
	
	priv->engine = engine;
	engine->priv = priv;
	db_engine_resize(engine, 0);
	
	return priv;
}

static void db_engine_private_free(db_engine_private_t * priv)
{
	if(NULL == priv) return;
	fprintf(stderr, "%s(%p)...\n", __FUNCTION__, priv);
	
	if(priv->env)
	{
		for(int i = 0; i < priv->count; ++i)
		{
			db_handle_t * db = priv->databases[i];
			if(db)
			{
				db_handle_cleanup(db);
				free(db);
				priv->databases[i] = NULL;
			}
		}
		priv->env->close(priv->env, 0);
		priv->env = NULL;
	}
	priv->count = 0;
	
	free(priv->databases);
	priv->databases = NULL;
	priv->max_size = 0;
	
	pthread_mutex_destroy(&priv->mutex);
	free(priv);
}

static int engine_set_home(struct db_engine * engine, const char * home_dir)
{
	int rc = -1;
	if(NULL == home_dir) home_dir = "./data";
	db_engine_private_t * priv = engine->priv;
	DB_ENV * env = priv->env;
	
	if(NULL == env)
	{
		rc = db_env_create(&priv->env, 0);
		db_check_error(rc, "%s() failed.", __FUNCTION__);
		assert(0 == rc);
		
		env = priv->env;
	}
	
	rc = env->open(env, home_dir, priv->env_flags, 0);
	db_check_error(rc, "%s() failed.", __FUNCTION__);
	assert(0 == rc);
	
	strncpy(priv->home_dir, home_dir, sizeof(priv->home_dir));
	return 0;
}

static db_handle_t * engine_open_db(struct db_engine * engine, const char * db_name, enum db_format_type db_type, int flags)
{
	assert(engine && engine->priv);
	int rc = -1;
	db_handle_t * db = db_handle_init(NULL, engine, engine->user_data);
	assert(db);
	
	rc = db->open(db, NULL, db_name, db_type, flags);
	db_check_error(rc, "%s() failed.", __FUNCTION__);
	assert(0 == rc);
	
	db_engine_private_t * priv = engine->priv;
	pthread_mutex_lock(&priv->mutex);
	
	db_engine_resize(engine, priv->count + 1);
	priv->databases[priv->count++] = db;
	
	pthread_mutex_unlock(&priv->mutex);
	return db;
}

static int engine_close_db(db_engine_t * engine, db_handle_t * db)
{
	int rc = -1;
	if(NULL == db) return -1;
	assert(engine && engine->priv);
	
	db_engine_private_t * priv = engine->priv;
	
	rc = db->close(db);
	db_check_error(rc, "%s() failed.", __FUNCTION__);
	
	pthread_mutex_lock(&priv->mutex);
	for(ssize_t i = 0; i < priv->count; ++i)
	{
		if(priv->databases[i] == db) {
			priv->databases[i] = priv->databases[--priv->count];
			priv->databases[priv->count] = NULL;
			break;
		}
	}
	
	pthread_mutex_unlock(&priv->mutex);
	return rc;
}

static db_engine_txn_t * engine_txn_new(struct db_engine * engine, struct db_engine_txn * parent_txn)
{
	db_engine_txn_t * txn = db_engine_txn_init(NULL, engine);
	assert(txn);
	
	int rc = txn->begin(txn, parent_txn);
	if(rc)
	{
		db_check_error(rc, "%s() failed.", __FUNCTION__);
		db_engine_txn_cleanup(txn);
		free(txn);
		txn = NULL;
	}
	return txn;
}
static void engine_txn_free(struct db_engine * engine, db_engine_txn_t * txn)
{
	if(txn)
	{
		db_engine_txn_cleanup(txn);
		free(txn);
	}
	return;
}


static db_engine_t g_db_engine[1] = {{
	.set_home = engine_set_home,
	.open_db = engine_open_db,
	.close_db = engine_close_db,
	.txn_new = engine_txn_new,
	.txn_free = engine_txn_free,
}};
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
#define global_lock() 		pthread_mutex_lock(&g_mutex)
#define global_unlock()		pthread_mutex_unlock(&g_mutex)

db_engine_t * db_engine_get() { return g_db_engine; }
static inline db_engine_t * db_engine_add_ref(db_engine_t * engine)  { 
	assert(engine);
	
	global_lock();
	db_engine_private_t * priv = engine->priv;
	if(NULL == priv) // the engine has already been destroyed
	{
		global_unlock();
		return NULL;
	}

	++priv->refs_count; 
	global_unlock();
	return engine;
} 
#define db_engine_unref(engine)		db_engine_cleanup(engine)

db_engine_t * db_engine_init(const char * home_dir, void * user_data)
{
	db_engine_t * engine = g_db_engine;
	engine->user_data = user_data;
	db_engine_private_t * priv = db_engine_private_new(engine);
	assert(priv && engine->priv == priv);
	engine->set_home(engine, home_dir);
	
	db_engine_add_ref(engine);
	return engine;
}

void db_engine_cleanup(db_engine_t * engine)
{
	if(NULL == engine || NULL == engine->priv) return;
	
	global_lock();
	db_engine_private_t * priv = engine->priv;
	if(NULL == priv || 0 == priv->refs_count) {
		global_unlock();
		return;
	}
	
	if(--priv->refs_count == 0)
	{
		db_engine_private_free(engine->priv);
		engine->priv = NULL;
	}
	global_unlock();
	return;
}

#if defined(_TEST_DB_ENGINE) && defined(_STAND_ALONE)

#include "satoshi-types.h"
struct db_record_block_data
{
	// block info
	struct satoshi_block_header hdr;
	int32_t txn_count;
	int32_t height;			// index of the secondary_db
	
	// block.dat file info
	int64_t file_index;
	int64_t start_pos;		// the begining of the block_data (just after block_file_hdr{magic, size} )
	
	// used to verify block_file_hdr : assert( start_pos >= 8 && (*(uint32_t *)(start-8) == magic)  && (*(uint32_t *)(start-4) == block_size) );
	uint32_t magic;
	uint32_t block_size;
}__attribute__((packed));

static ssize_t associate_blocks_height(db_handle_t * db, 
	const db_record_data_t * key, 
	const db_record_data_t * value, 
	db_record_data_t ** p_result)
{
	static const int num_results = 1;
	if(NULL == p_result) return num_results;	// returns the required array size for custom memory allocator
	
	db_record_data_t * result = * p_result;
	if(NULL == result) {
		result = calloc(1, sizeof(*result));
		*p_result = result;
	}
	
	struct db_record_block_data * block = (struct db_record_block_data *)value->data;
	assert(sizeof(*block) == value->size);
	
	result->data = &block->height;
	result->size = sizeof(block->height);
	return num_results;
}

int main(int argc, char **argv)
{
	db_engine_t * engine = db_engine_init("data", NULL);
	assert(engine);
	
	// create primary_db and secondary_db
	db_handle_t * sdb = engine->open_db(engine, "blocks_height.db", db_format_type_btree, 0);
	db_handle_t * db = engine->open_db(engine, "blocks.db", db_format_type_btree, 0);
	assert(sdb && db);
	int rc = db->associate(db, NULL, sdb, associate_blocks_height);
	assert(0 == rc);
	
	// add records 
	unsigned char hash[32] = { 0 };
	struct db_record_block_data block[1];
	
	db->flags = db_record_flags_no_overwrite;
	
	for(int i = 0; i < 10; ++i)
	{	
		*(int *)hash = 1000 + i;
		memset(block, 0, sizeof(block));
		block->height = i;
		
		rc = db->insert(db, NULL,
			&(db_record_data_t){.data = hash, .size = sizeof(hash) },
			&(db_record_data_t){.data = block, .size = sizeof(block)}
		);
		if(rc == DB_KEYEXIST) break;
		assert(0 == rc);
	}
	
	// dump records
	DBC * cursor = NULL;
	DB * dbp = db_get_handle(db);
	DB * sdbp = db_get_handle(sdb);
	
	assert(dbp && sdbp);
	rc = dbp->cursor(dbp, NULL, &cursor, DB_READ_COMMITTED);
	db_check_error(rc, "db->cursor() failed");
	assert(0 == rc);
	
	DBT key, value;
	memset(&key, 0, sizeof(key));
	memset(&value, 0, sizeof(value));
	
	memset(hash, 0, sizeof(hash));
	key.flags = DB_DBT_USERMEM;
	key.data = hash;
	key.ulen = 32;
	
	
	rc = cursor->get(cursor, &key, &value, DB_FIRST);
	assert(0 == rc);
	
	while(0 == rc)
	{
		struct db_record_block_data * data = value.data;
		assert(data && value.size == sizeof(*data));
		
		printf("key: %d, value: height=%d\n", *(int *)hash, data->height);
		rc = cursor->get(cursor, &key, &value, DB_NEXT);
		db_check_error(rc, "cursor->get()\n");
	}
	
	assert(rc == DB_NOTFOUND);
	cursor->close(cursor);
	
	
	
// test add_ref / unref
	db_engine_add_ref(engine);
	db_engine_cleanup(engine);
	db_engine_cleanup(engine);
	db_engine_cleanup(engine);
	return 0;
}
#endif
