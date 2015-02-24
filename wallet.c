/*

 Copyright (c) 2015 Douglas J. Bakkum

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.

*/

/* Some functions adapted from the Trezor crypto library. */


#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "commander.h"
#include "ripemd160.h"
#include "wallet.h"
#include "memory.h"
#include "random.h"
#include "base64.h"
#include "base58.h"
#include "pbkdf2.h"
#include "utils.h"
#include "bip32.h"
#include "hmac.h"
#include "sha2.h"
#include "uECC.h"
#include "led.h"

#include "bip39_english.h"

extern const uint8_t MEM_PAGE_ERASE[MEM_PAGE_LEN];
extern const uint16_t MEM_PAGE_ERASE_2X[MEM_PAGE_LEN];

static char mnemonic[256];      // longer than max wordlength+1  *  max numwords  +  1 
						        //		for bip32/39_english -> (8+1)*24+1 = 217
static uint16_t seed_index[25]; // longer than max numwords + 1
static uint8_t seed[64];
static uint8_t rand_data_32[32];


// Avoid leaving secrets in RAM
static void clear_static_variables(void)
{
    memset(seed, 0, sizeof(seed));
    memset(mnemonic, 0, sizeof(mnemonic));
    memset(seed_index, 0, sizeof(seed_index));
    memset(rand_data_32, 0, sizeof(rand_data_32));
}


static int split_seed(char **seed_words, const char *message)
{
    int i = 0;
    static char msg[256];
    
    memset(msg, 0, 256);
    memcpy(msg, message, strlen(message));
    seed_words[i] = strtok(msg, " ,");
    for (i = 0; seed_words[i] != NULL; seed_words[++i] = strtok(NULL, " ,")) { }
    return i;
}


const char **wallet_mnemonic_wordlist(void)
{
	return wordlist;
}


uint16_t *wallet_index_from_mnemonic(const char *mnemo)
{
    int i, j, k, seed_words_n;
    char *seed_word[25] = {NULL}; 
    memset(seed_index, 0, sizeof(seed_index));
    seed_words_n = split_seed(seed_word, mnemo);
   
    k = 0;
    for (i = 0; i < seed_words_n; i++) {
        for (j = 0; wordlist[j]; j++) {
            if (strcmp(seed_word[i], wordlist[j]) == 0) { // word found
                seed_index[k++] = j + 1; // offset of 1
                break;
            }
        }
	}
    return seed_index;
}


char *wallet_mnemonic_from_index(const uint16_t *idx)
{
    if (!memcmp(idx, MEM_PAGE_ERASE_2X, 64)) {
       return NULL;
    }
    int i;
    memset(mnemonic, 0, sizeof(mnemonic));
    for (i = 0; idx[i]; i++) {
        strcat(mnemonic, wordlist[idx[i] - 1]);
        strcat(mnemonic, " ");
    }
    mnemonic[strlen(mnemonic)-1] = '\0';
    return mnemonic;
}


void wallet_master_from_mnemonic(char *mnemo, int m_len, const char *salt, int s_len, int strength)
{
    HDNode node;

    clear_static_variables();

    if (mnemo == NULL) {
        if (!strength) { strength = 256; }
	    if (strength % 32 || strength < 128 || strength > 256) {
            commander_fill_report("seed", "Strength must be a multiple of 32 between 128 and 256.", ERROR); 
		    return;
	    }
        random_bytes(rand_data_32, 32, 1);
	    mnemo = wallet_mnemonic_from_data(rand_data_32, strength / 8);
		memcpy(mnemonic, mnemo, strlen(mnemo));
    } else {
		memcpy(mnemonic, mnemo, m_len);
	}

    if (wallet_mnemonic_check(mnemonic) == 0) {
        // error report is filled inside mnemonic_check()
        return;
    }

    if (salt == NULL) {
        wallet_mnemonic_to_seed(mnemonic, "", seed, 0); 
    } else { 
		char s[s_len];
		memcpy(s, salt, s_len);
        wallet_mnemonic_to_seed(mnemonic, s, seed, 0); 
    } 

	hdnode_from_seed(seed, sizeof(seed), &node);
    
    if (!memcmp(memory_master(node.private_key), MEM_PAGE_ERASE, 32)  ||
        !memcmp(memory_chaincode(node.chain_code), MEM_PAGE_ERASE, 32) ||
        !memcmp(memory_mnemonic(wallet_index_from_mnemonic(mnemonic)), MEM_PAGE_ERASE_2X, 64)) {    
        commander_fill_report("seed", "Problem saving BIP32 master key.", ERROR); 
    } else {
        commander_fill_report("seed", "success", SUCCESS);
    }
    clear_static_variables();
}


void wallet_generate_key(HDNode *node, char *key_path, const uint8_t *privkeymaster, const uint8_t *chaincode)
{
    unsigned long idx;
    char *pch;
   
    node->depth = 0;
    node->child_num = 0;
	node->fingerprint = 0x00000000;
    memcpy(node->chain_code, chaincode, 32);
    memcpy(node->private_key, privkeymaster, 32);
	hdnode_fill_public_key(node);
    
    pch = strtok(key_path, " /,m\\");
    while (pch != NULL) {
        sscanf(pch, "%lu", &idx); 
        if (pch[strlen(pch)-1] == '\'' ||
            pch[strlen(pch)-1] == 'p'  ||
            pch[strlen(pch)-1] == 'h'  ||
            pch[strlen(pch)-1] == 'H') {
            hdnode_private_ckd_prime(node, idx); 
        } else {
            hdnode_private_ckd(node, idx); 
        }
        pch = strtok(NULL, " /,m\\");
    } 
}


void wallet_report_xpub(char *keypath)
{
	char xpub[112];
    uint8_t *priv_key_master = memory_master(NULL);
    uint8_t *chain_code = memory_chaincode(NULL);
    HDNode node; 

    if (!memcmp(priv_key_master, MEM_PAGE_ERASE, 32) || 
        !memcmp(chain_code, MEM_PAGE_ERASE, 32)) {
        commander_fill_report("xpub", "A bip32 master private key is not set.", ERROR);
    } else {
        wallet_generate_key(&node, keypath, priv_key_master, chain_code);
	    hdnode_serialize_public(&node, xpub, sizeof(xpub));
        commander_fill_report("xpub", xpub, SUCCESS);
    }
    clear_static_variables();
}    


void wallet_sign(const char *message, int msg_len, char *keypath)
{
    uint8_t data[32];
    uint8_t sig[64];
    uint8_t *priv_key_master = memory_master(NULL);
    uint8_t *chain_code = memory_chaincode(NULL);
	uint8_t pub_key[33];
    HDNode node;
    
    if (msg_len != (32 * 2)) 
    {
        commander_fill_report("sign", "Incorrect data length. "
                    "A 32-byte hexadecimal value (64 characters) is expected.", ERROR);
    } 
    else if (!memcmp(priv_key_master, MEM_PAGE_ERASE, 32) ||
        !memcmp(chain_code, MEM_PAGE_ERASE, 32)) 
    {    
        commander_fill_report("sign", "A BIP32 master private key is not set.", ERROR); 
    } 
    else 
    {
        memcpy(data, hex_to_uint8(message), 32);
        wallet_generate_key(&node, keypath, priv_key_master, chain_code);
        if (!uECC_sign_digest(node.private_key, data, sig)) {
            commander_fill_report("sign", "Could not sign data.", ERROR);
        } else {
            uECC_get_public_key33(node.private_key, pub_key);
            commander_fill_report("sign", uint8_to_hex(sig, 64), SUCCESS);
	        commander_fill_report("pubkey", uint8_to_hex(pub_key, 33), SUCCESS);
        }
    }
    clear_static_variables();
}


char *wallet_mnemonic_from_data(const uint8_t *data, int len)
{
	if (len % 4 || len < 16 || len > 32) {
		return 0;
	}

	uint8_t bits[32 + 1];

	sha256_Raw(data, len, bits);
	bits[len] = bits[0];
	memcpy(bits, data, len);

	int mlen = len * 3 / 4;
	static char mnemo[24 * 10];

	int i, j, idx;
	char *p = mnemo;
	for (i = 0; i < mlen; i++) {
		idx = 0;
		for (j = 0; j < 11; j++) {
			idx <<= 1;
			idx += (bits[(i * 11 + j) / 8] & (1 << (7 - ((i * 11 + j) % 8)))) > 0;
		}
		strcpy(p, wordlist[idx]);
		p += strlen(wordlist[idx]);
		*p = (i < mlen - 1) ? ' ' : 0;
		p++;
	}

	return mnemo;
}


int wallet_mnemonic_check(const char *mnemo)
{
	if (!mnemo) {
        commander_fill_report("seed", "Empty mnemonic.", ERROR);
		return 0;
	}

	uint32_t i, n;
	
    // check number of words
    char *sham[25] = {NULL}; 
    n = split_seed(sham, mnemo);
    memset(sham, 0, sizeof(sham)); 
    
    
    if (n != 12 && n != 18 && n != 24) {
        commander_fill_report("seed", "Mnemonic must have 12, 18, or 24 words.", ERROR);
		return 0;
	}

	char current_word[10];
	uint32_t j, k, ki, bi;
	uint8_t bits[32 + 1];
	memset(bits, 0, sizeof(bits));
	i = 0; bi = 0;
	while (mnemo[i]) {
		j = 0;
		while (mnemo[i] != ' ' && mnemo[i] != 0) {
			if (j >= sizeof(current_word)) {
                commander_fill_report("seed", "Word not in bip39 wordlist.", ERROR);
				return 0;
			}
			current_word[j] = mnemo[i];
			i++; j++;
		}
		current_word[j] = 0;
		if (mnemo[i] != 0) i++;
		k = 0;
		for (;;) {
			if (!wordlist[k]) { // word not found
                commander_fill_report("seed", "Word not in bip39 wordlist.", ERROR);
				return 0;
			}
			if (strcmp(current_word, wordlist[k]) == 0) { // word found on index k
				for (ki = 0; ki < 11; ki++) {
					if (k & (1 << (10 - ki))) {
						bits[bi / 8] |= 1 << (7 - (bi % 8));
					}
					bi++;
				}
				break;
			}
			k++;
		}
	}
	if (bi != n * 11) {
        commander_fill_report("seed", "Mnemonic check error. [0]", ERROR);
		return 0;
	}
	bits[32] = bits[n * 4 / 3];
	sha256_Raw(bits, n * 4 / 3, bits);
	if (n == 12) {
		return (bits[0] & 0xF0) == (bits[32] & 0xF0); // compare first 4 bits
	} else
	if (n == 18) {
		return (bits[0] & 0xFC) == (bits[32] & 0xFC); // compare first 6 bits
	} else
	if (n == 24) {
		return bits[0] == bits[32]; // compare 8 bits
	}
   
    commander_fill_report("seed", "Invalid mnemonic: checksum error.", ERROR);
    return 0;
}


void wallet_mnemonic_to_seed(const char *mnemo, const char *passphrase, uint8_t s[512 / 8],
                             void (*progress_callback)(uint32_t current, uint32_t total))
{
	static uint8_t salt[8 + 256 + 4];
	int saltlen = strlen(passphrase);
	memcpy(salt, "mnemonic", 8);
	memcpy(salt + 8, passphrase, saltlen);
	saltlen += 8;
	pbkdf2_hmac_sha512((const uint8_t *)mnemo, strlen(mnemo), salt, saltlen, BIP39_PBKDF2_ROUNDS, s, 512 / 8, progress_callback);
}



// -- bitcoin formats -- //
// from: github.com/trezor/trezor-crypto 

void wallet_get_pubkeyhash(const uint8_t *pub_key, uint8_t *pubkeyhash)
{
	uint8_t h[32];
	if (pub_key[0] == 0x04) {        // uncompressed format
		sha256_Raw(pub_key, 65, h);
	} else if (pub_key[0] == 0x00) { // point at infinity
		sha256_Raw(pub_key, 1, h);
	} else {
		sha256_Raw(pub_key, 33, h);  // expecting compressed format
	}
	ripemd160(h, 32, pubkeyhash);
}

void wallet_get_address_raw(const uint8_t *pub_key, uint8_t version, uint8_t *addr_raw)
{
	addr_raw[0] = version;
	wallet_get_pubkeyhash(pub_key, addr_raw + 1);
}

void wallet_get_address(const uint8_t *pub_key, uint8_t version, char *addr, int addrsize)
{
	uint8_t raw[21];
	wallet_get_address_raw(pub_key, version, raw);
	base58_encode_check(raw, 21, addr, addrsize);
}

void wallet_get_wif(const uint8_t *priv_key, uint8_t version, char *wif, int wifsize)
{
	uint8_t data[34];
	data[0] = version;
	memcpy(data + 1, priv_key, 32);
	data[33] = 0x01;
	base58_encode_check(data, 34, wif, wifsize);
}

/*
int wallet_sig_to_der(const uint8_t *sig, uint8_t *der)
{
	int i;
	uint8_t *p = der, *len, *len1, *len2;
	*p = 0x30; p++;                        // sequence
	*p = 0x00; len = p; p++;               // len(sequence)

	*p = 0x02; p++;                        // integer
	*p = 0x00; len1 = p; p++;              // len(integer)

	// process R
	i = 0;
	while (sig[i] == 0 && i < 32) { i++; } // skip leading zeroes
	if (sig[i] >= 0x80) { // put zero in output if MSB set
		*p = 0x00; p++; *len1 = *len1 + 1;
	}
	while (i < 32) { // copy bytes to output
		*p = sig[i]; p++; *len1 = *len1 + 1; i++;
	}

	*p = 0x02; p++;                        // integer
	*p = 0x00; len2 = p; p++;              // len(integer)

	// process S
	i = 32;
	while (sig[i] == 0 && i < 64) { i++; } // skip leading zeroes
	if (sig[i] >= 0x80) { // put zero in output if MSB set
		*p = 0x00; p++; *len2 = *len2 + 1;
	}
	while (i < 64) { // copy bytes to output
		*p = sig[i]; p++; *len2 = *len2 + 1; i++;
	}

	*len = *len1 + *len2 + 4;
	return *len + 2;
}
*/