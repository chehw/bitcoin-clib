#ifndef _SATOSHI_TX_H_
#define _SATOSHI_TX_H_

#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

#include "satoshi-types.h"
#include "sha.h"

/*****************************************
 * satoshi_tx_sighash_type:
 * 	sighash_all: 	default type, signs all txins and txouts.
 * 	sighash_none:	signs all txins, no txouts, allowing anyone to change output amounts. (an Unfilled Signed Cheque).
 * 	sighash_single: sign all txins and txouts[cur_index] only. to ensure nobody can change txouts[cur_index]
 * 	
 * sighash_all    | sighash_anyone_can_pay: signs txins[cur_index] and all txouts, allows anyone to add or remove other inputs.
 * sighash_none   | sighash_anyone_can_pay: signs txins[cur_index] only, allows anyone to add or remove other inputs or outputs.
 * sighash_single | sighash_anyone_can_pay:  signs txins[cur_index] and txouts[cur_index]. to ensure nobody can change txouts[cur_index], and allows anyone to add or remove other inputs.
 * 
*****************************************/
enum satoshi_tx_sighash_type
{
	satoshi_tx_sighash_all = 1,							
	satoshi_tx_sighash_none = 2,
	satoshi_tx_sighash_single = 3,
	satoshi_tx_sighash_anyone_can_pay = 0x80
};

/*******************************************
 * satoshi_rawtx:
 * 	generate digest for sign / verify 
*******************************************/
typedef struct satoshi_rawtx 
{
	satoshi_tx_t * tx;	// attached tx
	satoshi_txin_t * txins;

	// internal states: pre-hash <-- sha(common_data)
	sha256_ctx_t sha[1];
	
	// virtual function
	int (* get_digest)(struct satoshi_rawtx * rawtx, 
		ssize_t cur_index, // current txin index
		uint32_t hash_type, 
		const satoshi_txout_t * utxo, 
		uint256_t * digest);

	// legacy tx states:
	int last_hashed_txin_index;	// pre-hashed index
	
	// segwit-v0 states:
	unsigned char txouts_hash[32]; // segwit_v0: generate preiamge step 8 
}satoshi_rawtx_t;

satoshi_rawtx_t * satoshi_rawtx_attach(satoshi_rawtx_t * rawtx, satoshi_tx_t * tx);
void satoshi_rawtx_detach(satoshi_rawtx_t * rawtx);

int satoshi_tx_get_digest(
	satoshi_tx_t * tx, 
	int txin_index, 
	uint32_t hash_type,
	const satoshi_txout_t * utxo,
	uint256_t * hash);

varstr_t * satoshi_txin_get_redeem_scripts(int is_segwit, const satoshi_txout_t * utxo);
void satoshi_tx_dump(const satoshi_tx_t * tx);

/**
 * @deprecated 
 * 	keep these functions for test use only
 * @{
 */
int segwit_v0_tx_get_digest(const satoshi_tx_t * tx, 
	int cur_index, 		// txin index
	uint32_t hash_type,
	const satoshi_txout_t * utxo, // prevout
	uint256_t * hash
);
/**
 * @}
 */


#ifdef __cplusplus
}
#endif
#endif
