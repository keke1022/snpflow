/* SPDX-License-Identifier: BSD-3-Clause */

#include "cvm_chunk_crypto.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

static int
hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int
cvm_chunk_key_parse(uint8_t key[CVM_CHUNK_KEY_SIZE], const char *hex)
{
	size_t i;

	if (key == NULL)
		return -EINVAL;
	if (hex == NULL || strlen(hex) != CVM_CHUNK_KEY_SIZE * 2) {
		OPENSSL_cleanse(key, CVM_CHUNK_KEY_SIZE);
		return -EINVAL;
	}
	for (i = 0; i < CVM_CHUNK_KEY_SIZE; i++) {
		int high = hex_nibble(hex[i * 2]);
		int low = hex_nibble(hex[i * 2 + 1]);

		if (high < 0 || low < 0) {
			OPENSSL_cleanse(key, CVM_CHUNK_KEY_SIZE);
			return -EINVAL;
		}
		key[i] = (uint8_t)((high << 4) | low);
	}
	return 0;
}

int
cvm_chunk_seal(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const struct cvm_chunk_header *header,
		const uint8_t *plaintext, size_t plaintext_len,
		uint8_t *ciphertext, uint8_t tag[CVM_CHUNK_TAG_SIZE])
{
	uint8_t aad[CVM_CHUNK_HEADER_SIZE];
	uint8_t nonce[CVM_CHUNK_NONCE_SIZE];
	EVP_CIPHER_CTX *ctx = NULL;
	int written = 0;
	int final_written = 0;
	int ret = -EIO;

	if (key == NULL || header == NULL || plaintext == NULL ||
	    ciphertext == NULL || tag == NULL || plaintext_len > INT_MAX ||
	    plaintext_len != header->payload_len ||
	    header->message_seq > CVM_CHUNK_MAX_MESSAGE_SEQ ||
	    (header->flags != CVM_CHUNK_F_DATA &&
	     header->flags != CVM_CHUNK_F_ACK))
		return -EINVAL;
	cvm_chunk_header_encode(aad, header);
	cvm_chunk_nonce(nonce, header);
	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL)
		goto out;
	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
				CVM_CHUNK_NONCE_SIZE, NULL) != 1 ||
	    EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1 ||
	    EVP_EncryptUpdate(ctx, NULL, &written, aad, sizeof(aad)) != 1 ||
	    EVP_EncryptUpdate(ctx, ciphertext, &written, plaintext,
			      (int)plaintext_len) != 1 ||
	    written != (int)plaintext_len ||
	    EVP_EncryptFinal_ex(ctx, ciphertext + written, &final_written) != 1 ||
	    final_written != 0 ||
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
				CVM_CHUNK_TAG_SIZE, tag) != 1)
		goto out;
	ret = 0;
out:
	if (ret != 0) {
		OPENSSL_cleanse(ciphertext, plaintext_len);
		OPENSSL_cleanse(tag, CVM_CHUNK_TAG_SIZE);
	}
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(aad, sizeof(aad));
	OPENSSL_cleanse(nonce, sizeof(nonce));
	return ret;
}

int
cvm_chunk_open(const uint8_t key[CVM_CHUNK_KEY_SIZE],
		const struct cvm_chunk_header *header,
		const uint8_t *ciphertext, size_t ciphertext_len,
		const uint8_t tag[CVM_CHUNK_TAG_SIZE], uint8_t *plaintext)
{
	uint8_t aad[CVM_CHUNK_HEADER_SIZE];
	uint8_t nonce[CVM_CHUNK_NONCE_SIZE];
	uint8_t expected_tag[CVM_CHUNK_TAG_SIZE];
	EVP_CIPHER_CTX *ctx = NULL;
	int written = 0;
	int final_written = 0;
	int ret = -EBADMSG;

	if (key == NULL || header == NULL || ciphertext == NULL || tag == NULL ||
	    plaintext == NULL || ciphertext_len > INT_MAX ||
	    ciphertext_len != header->payload_len ||
	    header->message_seq > CVM_CHUNK_MAX_MESSAGE_SEQ ||
	    (header->flags != CVM_CHUNK_F_DATA &&
	     header->flags != CVM_CHUNK_F_ACK))
		return -EINVAL;
	cvm_chunk_header_encode(aad, header);
	cvm_chunk_nonce(nonce, header);
	memcpy(expected_tag, tag, sizeof(expected_tag));
	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		ret = -EIO;
		goto out;
	}
	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
				CVM_CHUNK_NONCE_SIZE, NULL) != 1 ||
	    EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1 ||
	    EVP_DecryptUpdate(ctx, NULL, &written, aad, sizeof(aad)) != 1 ||
	    EVP_DecryptUpdate(ctx, plaintext, &written, ciphertext,
			      (int)ciphertext_len) != 1 ||
	    written != (int)ciphertext_len ||
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
				CVM_CHUNK_TAG_SIZE, expected_tag) != 1 ||
	    EVP_DecryptFinal_ex(ctx, plaintext + written, &final_written) != 1 ||
	    final_written != 0)
		goto out;
	ret = 0;
out:
	if (ret != 0)
		OPENSSL_cleanse(plaintext, ciphertext_len);
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(aad, sizeof(aad));
	OPENSSL_cleanse(nonce, sizeof(nonce));
	OPENSSL_cleanse(expected_tag, sizeof(expected_tag));
	return ret;
}
