/*
 * Copyright 2013-2015 Guardtime, Inc.
 *
 * This file is part of the Guardtime client SDK.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES, CONDITIONS, OR OTHER LICENSES OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 * "Guardtime" and "KSI" are trademarks or registered trademarks of
 * Guardtime, Inc., and no license to trademarks is granted; Guardtime
 * reserves and retains all trademark rights.
 */

#include <string.h>

#include "internal.h"
#include "fast_tlv.h"
#include "tlv.h"
#include "io.h"

#define KSI_BUFFER_SIZE 0xffff + 1

struct KSI_TLV_st {
	/** Context. */
	KSI_CTX *ctx;

	/** Reference count */
	unsigned refCount;

	/** Reference to parent TLV */
	KSI_TLV *parent;

	/** Flags */
	int isNonCritical;
	int isForwardable;

	/** TLV tag. */
	unsigned tag;

	/** Max size of the buffer. Default is 0xffff bytes. */
	size_t buffer_size;

	/** Internal storage. */
	unsigned char *buffer;

	/** Internal storage of nested TLV's. */
	KSI_LIST(KSI_TLV) *nested;

	/** How the payload is encoded internally. */
	int payloadType;

	unsigned char *datap;
	size_t datap_len;

	size_t relativeOffset;
	size_t absoluteOffset;

};

KSI_IMPLEMENT_LIST(KSI_TLV, KSI_TLV_free);

/**
 *
 */
static int createOwnBuffer(KSI_TLV *tlv, int copy) {
	int res = KSI_UNKNOWN_ERROR;
	unsigned char *buf = NULL;
	size_t buf_len = 0;

	if (tlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}
	KSI_ERR_clearErrors(tlv->ctx);

	if (tlv->buffer != NULL) {
		KSI_pushError(tlv->ctx, res = KSI_INVALID_ARGUMENT, "TLV buffer already allocated.");
		goto cleanup;
	}

	buf = KSI_malloc(KSI_BUFFER_SIZE);
	if (buf == NULL) {
		KSI_pushError(tlv->ctx, res = KSI_OUT_OF_MEMORY, NULL);
		goto cleanup;
	}

	if (copy && tlv->datap != NULL) {
		buf_len = tlv->datap_len;

		memcpy(buf, tlv->datap, buf_len);
	}

	tlv->buffer = buf;
	buf = NULL;

	tlv->datap = tlv->buffer;
	tlv->datap_len = buf_len;

	tlv->buffer_size = KSI_BUFFER_SIZE;

	res = KSI_OK;

cleanup:

	KSI_free(buf);

	return res;
}

static int readHeader(KSI_RDR *rdr, unsigned char *dest, size_t *headerLen, int *isNonCritical, int *isForward, unsigned *tag, size_t *length) {
	int res = KSI_UNKNOWN_ERROR;
	size_t readCount;

	if (rdr == NULL || dest == NULL || headerLen == NULL) {
		KSI_pushError(KSI_RDR_getCtx(rdr), res = KSI_INVALID_ARGUMENT, "One of the arguments was null.");
		goto cleanup;
	}

	/* Read first two bytes */
	res = KSI_RDR_read_ex(rdr, dest, 2, &readCount);
	if (res != KSI_OK) goto cleanup;

	if (readCount == 0 && KSI_RDR_isEOF(rdr)) {
		/* Reached end of stream. */
		*headerLen = 0;
		res = KSI_OK;
		goto cleanup;
	}
	if (readCount != 2) {
		KSI_pushError(KSI_RDR_getCtx(rdr), res = KSI_INVALID_FORMAT, "Unable to read first two bytes.");
		goto cleanup;
	}

	if (isNonCritical != NULL) *isNonCritical = *dest & KSI_TLV_MASK_LENIENT;
	if (isForward != NULL) *isForward = *dest & KSI_TLV_MASK_FORWARD;

	/* Is it a TLV8 or TLV16 */
	if (*dest & KSI_TLV_MASK_TLV16) {
		/* TLV16 */
		/* Read additional 2 bytes of header */
		res = KSI_RDR_read_ex(rdr, dest + 2, 2, &readCount);
		if (res != KSI_OK) goto cleanup;
		if (readCount != 2) {
			KSI_pushError(KSI_RDR_getCtx(rdr), res = KSI_INVALID_FORMAT, "Unable to read full TLV16 header.");
			goto cleanup;
		}
		*headerLen = 4;

		if (tag != NULL) *tag = ((dest[0] & KSI_TLV_MASK_TLV8_TYPE) << 8 ) | dest[1];
		/* Added masking for Fortify. */
		if (length != NULL) *length = ((dest[2] << 8) | (unsigned)dest[3]) & 0xffff;
	} else {
		/* TLV8 */
		*headerLen = 2;
		if (tag != NULL) *tag = dest[0] & KSI_TLV_MASK_TLV8_TYPE;
		if (length != NULL) *length = dest[1];
	}

	res = KSI_OK;

cleanup:

	return res;
}

/**
 *
 */
static int encodeAsRaw(KSI_TLV *tlv) {
	int res = KSI_UNKNOWN_ERROR;
	size_t payloadLength;
	unsigned char *buf = NULL;
	size_t buf_size = 0;

	if (tlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}
	KSI_ERR_clearErrors(tlv->ctx);

	if (tlv->payloadType == KSI_TLV_PAYLOAD_RAW) {
		res = KSI_OK;
		goto cleanup;
	}

	if (tlv->buffer == NULL) {
		buf_size = 0xffff + 1;
		buf = KSI_calloc(buf_size, 1);
		if (buf == NULL) {
			KSI_pushError(tlv->ctx, res = KSI_OUT_OF_MEMORY, NULL);
			goto cleanup;
		}
	} else {
		buf = tlv->buffer;
		buf_size = tlv->buffer_size;
	}

	payloadLength = buf_size;
	res = KSI_TLV_serializePayload(tlv, buf, &payloadLength);
	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}

	tlv->payloadType = KSI_TLV_PAYLOAD_RAW;
	tlv->buffer = buf;
	tlv->buffer_size = buf_size;

	tlv->datap = buf;
	tlv->datap_len = payloadLength;

	KSI_TLVList_free(tlv->nested);
	tlv->nested = NULL;

	buf = NULL;

	res = KSI_OK;

cleanup:

	KSI_free(buf);

	return res;
}

static size_t readFirstTlv(KSI_CTX *ctx, unsigned char *data, size_t data_length, KSI_TLV **tlv) {
	int res;
	size_t bytesConsumed = 0;

	KSI_TLV *tmp = NULL;
	KSI_FTLV ftlv;

	if (ctx == NULL || data == NULL || tlv == NULL || data_length == 0) {
		goto cleanup;
	}

	memset(&ftlv, 0, sizeof(KSI_FTLV));

	res = KSI_FTLV_memRead(data, data_length, &ftlv);
	if (res != KSI_OK) goto cleanup;

	res = KSI_TLV_new(ctx, KSI_TLV_PAYLOAD_RAW, ftlv.tag, ftlv.is_nc, ftlv.is_fwd, &tmp);
	if (res != KSI_OK) goto cleanup;

	tmp->datap = data + ftlv.hdr_len;
	tmp->datap_len = ftlv.dat_len;

	*tlv = tmp;
	tmp = NULL;

	bytesConsumed = ftlv.hdr_len + ftlv.dat_len;

cleanup:

	KSI_TLV_free(tmp);

	return bytesConsumed;
}


static int encodeAsNestedTlvs(KSI_TLV *tlv) {
	int res = KSI_UNKNOWN_ERROR;
	KSI_TLV *tmp = NULL;
	KSI_LIST(KSI_TLV) *tlvList = NULL;
	size_t allConsumedBytes = 0;
	size_t lastConsumedBytes = 0;

	if (tlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	if (tlv->payloadType == KSI_TLV_PAYLOAD_TLV) {
		res = KSI_OK;
		goto cleanup;
	}

	if (tlv->payloadType != KSI_TLV_PAYLOAD_RAW) {
		KSI_pushError(tlv->ctx, res = KSI_TLV_PAYLOAD_TYPE_MISMATCH, NULL);
		goto cleanup;
	}

	res = KSI_TLVList_new(&tlvList);
	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}

	/* Try parsing all of the nested TLV's. */
	while (allConsumedBytes < tlv->datap_len) {
		lastConsumedBytes = readFirstTlv(tlv->ctx, tlv->datap + allConsumedBytes, tlv->datap_len - allConsumedBytes, &tmp);

		if (tmp == NULL) {
			KSI_pushError(tlv->ctx, res = KSI_INVALID_FORMAT, NULL);
			goto cleanup;
		}

		/* Update the absolute offset of the child TLV object. */
		tmp->absoluteOffset += allConsumedBytes;

		allConsumedBytes += lastConsumedBytes;

		res = KSI_TLVList_append(tlvList, tmp);
		if (res != KSI_OK) {
			KSI_pushError(tlv->ctx, res, NULL);
			goto cleanup;
		}


		tmp = NULL;
	}

	if (allConsumedBytes > tlv->datap_len) {
		KSI_pushError(tlv->ctx, res = KSI_INVALID_FORMAT, NULL);
		goto cleanup;
	}

	tlv->payloadType = KSI_TLV_PAYLOAD_TLV;
	tlv->nested = tlvList;
	tlvList = NULL;

	res = KSI_OK;

cleanup:

	KSI_TLV_free(tmp);
	KSI_TLVList_free(tlvList);

	return res;
}

int KSI_TLV_setUintValue(KSI_TLV *tlv, KSI_uint64_t val) {
	int res = KSI_UNKNOWN_ERROR;
	size_t len;

	if (tlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	len = KSI_UINT64_MINSIZE(val);
	if (tlv->buffer == NULL) {
		res = createOwnBuffer(tlv, 0);
		if (res != KSI_OK) {
			KSI_pushError(tlv->ctx, res, NULL);
			goto cleanup;
		}
	}

	tlv->datap = tlv->buffer;
	tlv->datap_len = len;

	for (; len > 0; len--) {
		tlv->datap[len - 1] = (unsigned char) (val & 0xff);
		val >>= 8;
	}

	res = KSI_OK;

cleanup:

	return res;
}

int KSI_TLV_setRawValue(KSI_TLV *tlv, const void *data, size_t data_len) {
	int res = KSI_UNKNOWN_ERROR;

	if (tlv == NULL || (data == NULL && data_len != 0)) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	if (tlv->payloadType != KSI_TLV_PAYLOAD_RAW) {
		KSI_pushError(tlv->ctx, res = KSI_INVALID_ARGUMENT, "TLV not a raw type");
		goto cleanup;
	}

	if (data_len >= KSI_BUFFER_SIZE) {
		KSI_pushError(tlv->ctx, res = KSI_BUFFER_OVERFLOW, NULL);
		goto cleanup;
	}

	if (tlv->buffer == NULL && data != NULL && data_len != 0) {
		res = createOwnBuffer(tlv, 0);
		if (res != KSI_OK) {
			KSI_pushError(tlv->ctx, res, NULL);
			goto cleanup;
		}
	}

	tlv->datap = tlv->buffer;
	tlv->datap_len = data_len;

	/* Double check the boundaries. */
	if (tlv->buffer_size < data_len) {
		KSI_pushError(tlv->ctx, res = KSI_BUFFER_OVERFLOW, NULL);
		goto cleanup;
	}

	memcpy(tlv->datap, data, data_len);

	res = KSI_OK;

cleanup:

	return res;
}

/**
 *
 */
int KSI_TLV_new(KSI_CTX *ctx, int payloadType, unsigned tag, int isLenient, int isForward, KSI_TLV **tlv) {
	int res = KSI_UNKNOWN_ERROR;
	KSI_TLV *tmp = NULL;

	KSI_ERR_clearErrors(ctx);

	if (ctx == NULL || tlv == NULL) {
		KSI_pushError(ctx, res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	tmp = KSI_new(KSI_TLV);
	if (tmp == NULL) {
		KSI_pushError(ctx, res = KSI_OUT_OF_MEMORY, NULL);
		goto cleanup;
	}

	/* Initialize context. */
	tmp->ctx = ctx;
	tmp->tag = tag;
	/* Make sure the values are *only* 1 or 0. */
	tmp->isNonCritical = isLenient ? 1 : 0;
	tmp->isForwardable = isForward ? 1 : 0;

	tmp->nested = NULL;
	tmp->refCount = 1;
	tmp->parent = NULL;

	tmp->buffer_size = 0;
	tmp->buffer = NULL;

	tmp->payloadType = payloadType;
	tmp->datap_len = 0;
	tmp->datap = NULL;

	tmp->relativeOffset = 0;
	tmp->absoluteOffset = 0;

	/* Update the out parameter. */
	*tlv = tmp;
	tmp = NULL;

	res = KSI_OK;

cleanup:

	KSI_TLV_free(tmp);

	return res;
}

/**
 *
 */
void KSI_TLV_free(KSI_TLV *tlv) {
	if (tlv != NULL && --tlv->refCount == 0) {
		KSI_free(tlv->buffer);
		/* Free nested data */

		KSI_TLVList_free(tlv->nested);
		KSI_free(tlv);
	}
}

void KSI_TLV_ref(KSI_TLV *tlv) {
	if (tlv != NULL) {
		tlv->refCount++;
	}
}

int KSI_TLV_fromReader(KSI_RDR *rdr, KSI_TLV **tlv) {
	int res = KSI_UNKNOWN_ERROR;
	unsigned char buf[0xffff + 4];
	unsigned char *raw = NULL;
	size_t consumed = 0;
	KSI_TLV *tmp = NULL;
	size_t offset = 0;
	KSI_CTX *ctx = NULL;

	if (rdr == NULL || tlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}
	KSI_ERR_clearErrors(ctx = KSI_RDR_getCtx(rdr));
	KSI_RDR_getOffset(rdr, &offset);

	res = KSI_TLV_readTlv(rdr, buf, sizeof(buf), &consumed);
	if (res != KSI_OK) {
		KSI_pushError(ctx, res, NULL);
		goto cleanup;
	}

	if(consumed > UINT_MAX){
		KSI_pushError(ctx, res = KSI_INVALID_ARGUMENT, "Unable to parse more data than UINT_MAX.");
		goto cleanup;
	}

	if (consumed > 0) {
		raw = KSI_malloc(consumed);
		if (raw == NULL) {
			KSI_pushError(ctx, res = KSI_OUT_OF_MEMORY, NULL);
			goto cleanup;
		}
		memcpy(raw, buf, consumed);
		KSI_LOG_logBlob(ctx, KSI_LOG_DEBUG, "Last raw read:", raw, consumed);

		res = KSI_TLV_parseBlob2(KSI_RDR_getCtx(rdr), raw, (unsigned) consumed, 1, &tmp);
		if (res != KSI_OK) {
			KSI_pushError(ctx, res, NULL);
			goto cleanup;
		}

		raw = NULL;

		tmp->absoluteOffset = offset;
	}

	*tlv = tmp;
	tmp = NULL;

	res = KSI_OK;

cleanup:

	KSI_free(raw);
	KSI_TLV_free(tmp);

	return res;
}

int KSI_TLV_readTlv(KSI_RDR *rdr, unsigned char *buffer, size_t buffer_len, size_t *readCount) {
	int res = KSI_UNKNOWN_ERROR;
	size_t headerRead;
	size_t valueRead;
	size_t valueLength = 0;

	if (rdr == NULL || buffer == NULL || buffer_len < 4 || readCount == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(KSI_RDR_getCtx(rdr));

	res = readHeader(rdr, buffer, &headerRead, NULL, NULL, NULL, &valueLength);
	if (res != KSI_OK) {
		KSI_pushError(KSI_RDR_getCtx(rdr), res, NULL);
		goto cleanup;
	}

	if (valueLength + headerRead > buffer_len) {
		KSI_pushError(KSI_RDR_getCtx(rdr), res = KSI_BUFFER_OVERFLOW, NULL);
		goto cleanup;
	}

	res = KSI_RDR_read_ex(rdr, buffer + headerRead, (size_t)valueLength, &valueRead);
	if (res != KSI_OK) {
		KSI_pushError(KSI_RDR_getCtx(rdr), res, NULL);
		goto cleanup;
	}

	if (valueLength != valueRead) {
		KSI_pushError(KSI_RDR_getCtx(rdr), res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	*readCount = headerRead + valueRead;

	res = KSI_OK;

cleanup:

	return res;
}


/**
 *
 */
int KSI_TLV_getRawValue(KSI_TLV *tlv, const unsigned char **buf, size_t *len) {
	int res = KSI_UNKNOWN_ERROR;

	if (tlv == NULL || buf == NULL || len == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	/* Check payload type. */
	if (tlv->payloadType == KSI_TLV_PAYLOAD_TLV) {
		KSI_pushError(tlv->ctx, res = KSI_TLV_PAYLOAD_TYPE_MISMATCH, NULL);
		goto cleanup;
	}

	*buf = tlv->datap;
	*len = tlv->datap_len;

	res = KSI_OK;

cleanup:

	return res;
}

int KSI_TLV_getNestedList(KSI_TLV *tlv, KSI_LIST(KSI_TLV) **list) {
	int res = KSI_UNKNOWN_ERROR;

	if (tlv == NULL || list == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	/* Check payload type. */
	if (tlv->payloadType != KSI_TLV_PAYLOAD_TLV) {
		KSI_pushError(tlv->ctx, res = KSI_TLV_PAYLOAD_TYPE_MISMATCH, NULL);
		goto cleanup;
	}

	*list = tlv->nested;

	res = KSI_OK;

cleanup:

	return res;
}

int KSI_TLV_parseBlob2(KSI_CTX *ctx, unsigned char *data, size_t data_length, int ownMemory, KSI_TLV **tlv) {
	int res = KSI_UNKNOWN_ERROR;
	KSI_TLV *tmp = NULL;
	size_t consumedBytes = 0;

	if (ctx == NULL || data == NULL || data_length < 2 || tlv == NULL) {
		KSI_pushError(ctx, res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	if ((consumedBytes = readFirstTlv(ctx, data, data_length, &tmp)) != data_length) {
		KSI_pushError(ctx, res = KSI_INVALID_FORMAT, NULL);
		goto cleanup;
	}

	/* This should never happen, but if it does, the function readFirstTlv is flawed - we'll check it here to just be sure. */
	if (tmp == NULL) {
		KSI_pushError(ctx, res = KSI_UNKNOWN_ERROR, "Reading TLV failed.");
		goto cleanup;
	}

	/* If the memory should be owned by the TLV, store the pointer to free it after use. */
	if (ownMemory) {
		tmp->buffer = data;
		tmp->buffer_size = data_length;
	}

	*tlv = tmp;
	tmp = NULL;

	res = KSI_OK;

cleanup:

	KSI_TLV_free(tmp);

	return res;

}

/**
 *
 */
int KSI_TLV_parseBlob(KSI_CTX *ctx, const unsigned char *data, size_t data_length, KSI_TLV **tlv) {
	int res = KSI_UNKNOWN_ERROR;
	unsigned char *tmpDat = NULL;

	KSI_ERR_clearErrors(ctx);
	if (ctx == NULL || data == NULL || tlv == NULL) {
		KSI_pushError(ctx, res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	tmpDat = KSI_calloc(data_length, 1);
	if (tmpDat == NULL) {
		KSI_pushError(ctx, res = KSI_OUT_OF_MEMORY, NULL);
		goto cleanup;
	}
	memcpy(tmpDat, data, data_length);

	res = KSI_TLV_parseBlob2(ctx, tmpDat, data_length, 1, tlv);
	if (res != KSI_OK) {
		KSI_pushError(ctx, res, NULL);
		goto cleanup;
	}

	tmpDat = NULL;

	res = KSI_OK;

cleanup:

	KSI_free(tmpDat);

	return res;
}

/**
 *
 */
int KSI_TLV_cast(KSI_TLV *tlv, int payloadType) {
	int res = KSI_UNKNOWN_ERROR;

	if (tlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	if (tlv->payloadType == payloadType) {
		res = KSI_OK;
		goto cleanup;
	}

	switch (payloadType) {
		case KSI_TLV_PAYLOAD_RAW:
			res = encodeAsRaw(tlv);
			break;
		case KSI_TLV_PAYLOAD_TLV:
			res = encodeAsNestedTlvs(tlv);
			break;
		default:
			KSI_pushError(tlv->ctx, res = KSI_INVALID_ARGUMENT, "Unknown TLV payload encoding.");
			goto cleanup;
	}

	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}

	res = KSI_OK;

cleanup:

	return res;
}

/**
 *
 */
int KSI_TLV_fromUint(KSI_CTX *ctx, unsigned tag, int isLenient, int isForward, KSI_uint64_t uint, KSI_TLV **tlv) {
	int res = KSI_UNKNOWN_ERROR;
	KSI_TLV *tmp = NULL;

	KSI_ERR_clearErrors(ctx);
	if (ctx == NULL || tlv == NULL) {
		KSI_pushError(ctx, res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	res = KSI_TLV_new(ctx, KSI_TLV_PAYLOAD_INT, tag, isLenient, isForward, &tmp);
	if (res != KSI_OK) {
		KSI_pushError(ctx, res, NULL);
		goto cleanup;
	}

	res = KSI_TLV_setUintValue(tmp, uint);
	if (res != KSI_OK) {
		KSI_pushError(ctx, res, NULL);
		goto cleanup;
	}

	*tlv = tmp;
	tmp = NULL;

	res = KSI_OK;

cleanup:

	KSI_TLV_free(tmp);

	return res;
}

/**
 *
 */
int KSI_TLV_isNonCritical(KSI_TLV *tlv) {
	return tlv->isNonCritical;
}

/**
 *
 */
int KSI_TLV_isForward(KSI_TLV *tlv) {
	return tlv->isForwardable;
}

/**
 *
 */
unsigned KSI_TLV_getTag(KSI_TLV *tlv) {
	return tlv->tag;
}

int KSI_TLV_removeNestedTlv(KSI_TLV *target, KSI_TLV *tlv) {
	int res = KSI_UNKNOWN_ERROR;
	size_t *pos = NULL;

	if (target == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(target->ctx);

	if (tlv == NULL) {
		KSI_pushError(target->ctx, res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	res = KSI_TLVList_indexOf(target->nested, tlv, &pos);
	if (res != KSI_OK) {
		KSI_pushError(target->ctx, res, NULL);
		goto cleanup;
	}

	if (pos == NULL) {
		KSI_pushError(target->ctx, res = KSI_INVALID_ARGUMENT, "Nested TLV not found.");
		goto cleanup;
	}

	res = KSI_TLVList_remove(target->nested, *pos, NULL);
	if (res != KSI_OK) {
		KSI_pushError(target->ctx, res, NULL);
		goto cleanup;
	}

	res = KSI_OK;

cleanup:

	KSI_free(pos);

	return res;
}

int KSI_TLV_replaceNestedTlv(KSI_TLV *parentTlv, KSI_TLV *oldTlv, KSI_TLV *newTlv) {
	int res = KSI_UNKNOWN_ERROR;
	size_t *pos = NULL;

	if (parentTlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(parentTlv->ctx);

	if (oldTlv == NULL || newTlv == NULL) {
		KSI_pushError(parentTlv->ctx, res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	if (parentTlv->payloadType != KSI_TLV_PAYLOAD_TLV) {
		KSI_pushError(parentTlv->ctx, res = KSI_TLV_PAYLOAD_TYPE_MISMATCH, NULL);
		goto cleanup;
	}

	res = KSI_TLVList_indexOf(parentTlv->nested, oldTlv, &pos);
	if (res != KSI_OK) {
		KSI_pushError(parentTlv->ctx, res, NULL);
		goto cleanup;
	}

	if (pos == NULL) {
		KSI_pushError(parentTlv->ctx, res = KSI_INVALID_ARGUMENT, "Nested TLV not found.");
		goto cleanup;
	}

	res = KSI_TLVList_replaceAt(parentTlv->nested, *pos, newTlv);
	if (res != KSI_OK) {
		KSI_pushError(parentTlv->ctx, res, NULL);
		goto cleanup;
	}

	res = KSI_OK;

cleanup:
	KSI_free(pos);

	return res;
}


/**
 *
 */
int KSI_TLV_appendNestedTlv(KSI_TLV *target, KSI_TLV *tlv) {
	int res = KSI_UNKNOWN_ERROR;
	size_t *pos = NULL;
	KSI_LIST(KSI_TLV) *list = NULL;

	if (target == NULL || tlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}
	KSI_ERR_clearErrors(target->ctx);

	if (target->payloadType != KSI_TLV_PAYLOAD_TLV) {
		KSI_pushError(target->ctx, res = KSI_TLV_PAYLOAD_TYPE_MISMATCH, NULL);
		goto cleanup;
	}

	if (target->nested == NULL) {
		res = KSI_TLVList_new(&list);
		if (res != KSI_OK) {
			KSI_pushError(target->ctx, res, NULL);
			goto cleanup;
		}

		target->nested = list;
		list = NULL;
	}

	res = KSI_TLVList_append(target->nested, tlv);
	if (res != KSI_OK) {
		KSI_pushError(target->ctx, res, NULL);
		goto cleanup;
	}

	res = KSI_OK;

cleanup:

	KSI_free(pos);
	KSI_TLVList_free(list);

	return res;
}

static int serializeTlv(const KSI_TLV *tlv, unsigned char *buf, size_t buf_size, size_t *buf_len, int serializeHeader);

static int serializeRaw(const KSI_TLV *tlv, unsigned char *buf, size_t buf_size, size_t *buf_len) {
	int res = KSI_UNKNOWN_ERROR;
	size_t payloadLength;

	if (tlv == NULL || (buf == NULL && buf_size != 0) || buf_len == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	if (tlv->payloadType != KSI_TLV_PAYLOAD_RAW) {
		KSI_pushError(tlv->ctx, res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	payloadLength = tlv->datap_len;

	if (buf != NULL) {
		if (buf_size < payloadLength) {
			KSI_pushError(tlv->ctx, res = KSI_INVALID_ARGUMENT, NULL);
			goto cleanup;
		}
		memcpy(buf + buf_size - payloadLength, tlv->datap, payloadLength);
	}

	*buf_len = payloadLength;

	res = KSI_OK;

cleanup:

	return res;
}

static int serializeNested(const KSI_TLV *tlv, unsigned char *buf, size_t buf_size, size_t *buf_len) {
	int res = KSI_UNKNOWN_ERROR;
	size_t len = 0;

	if (tlv == NULL || (buf == NULL && buf_size != 0) || buf_len == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);


	if (tlv->payloadType != KSI_TLV_PAYLOAD_TLV) {
		KSI_pushError(tlv->ctx, res = KSI_INVALID_ARGUMENT, NULL);
		goto cleanup;
	}

	if (tlv->nested != NULL) {
		size_t i;
		size_t tmp_len = 0;

		for (i = KSI_TLVList_length(tlv->nested); i > 0; i--) {
			KSI_TLV *tmp = NULL;

			res = KSI_TLVList_elementAt(tlv->nested, i - 1, &tmp);
			if (res != KSI_OK) {
				KSI_pushError(tlv->ctx, res, NULL);
				goto cleanup;
			}

			res = serializeTlv(tmp, buf, (buf == NULL ? 0 : buf_size - len), &tmp_len, 0);
			if (res != KSI_OK) {
				KSI_pushError(tlv->ctx, res, NULL);
				goto cleanup;
			}

			len += tmp_len;
		}

	}

	*buf_len = len;

	res = KSI_OK;

cleanup:

	return res;
}

static int serializePayload(const KSI_TLV *tlv, unsigned char *buf, size_t buf_size, size_t *buf_len) {
	int res = KSI_UNKNOWN_ERROR;

	if (tlv == NULL || buf_len == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	switch (tlv->payloadType) {
		case KSI_TLV_PAYLOAD_RAW:
			res = serializeRaw(tlv, buf, buf_size, buf_len);
			break;
		case KSI_TLV_PAYLOAD_TLV:
			res = serializeNested(tlv, buf, buf_size, buf_len);
			break;
		default:
			KSI_pushError(tlv->ctx, res = KSI_UNKNOWN_ERROR, "Dont know how to serialize unknown payload type.");
			goto cleanup;
	}
	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}

	res = KSI_OK;

cleanup:

	return res;
}

static int serializeTlv(const KSI_TLV *tlv, unsigned char *buf, size_t buf_size, size_t *buf_len, int opt) {
	int res = KSI_UNKNOWN_ERROR;
	unsigned char *ptr = NULL;
	size_t len;
	size_t hdr_len = 0;

	if (tlv == NULL || (buf == NULL && buf_size != 0) || buf_len == NULL) {
		res = KSI_UNKNOWN_ERROR;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	res = serializePayload(tlv, buf, buf_size, &len);
	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}

	if (buf != NULL) {
		ptr = buf + buf_size - len - 1;
	}

	if ((opt & KSI_TLV_OPT_NO_HEADER) == 0) {
		/* Write header */
		if (len > 0xff || tlv->tag > KSI_TLV_MASK_TLV8_TYPE) {
			hdr_len = 4;

			if (ptr != NULL) {
				/* Encode as TLV16 */
				if (buf_size < hdr_len + len) {
					KSI_pushError(tlv->ctx, res = KSI_BUFFER_OVERFLOW, NULL);
					goto cleanup;
				}
				*ptr-- = 0xff & len;
				*ptr-- = 0xff & len >> 8;
				*ptr-- = tlv->tag & 0xff;
				*ptr-- = (unsigned char) (KSI_TLV_MASK_TLV16 | (tlv->isNonCritical ? KSI_TLV_MASK_LENIENT : 0) | (tlv->isForwardable ? KSI_TLV_MASK_FORWARD : 0) | (tlv->tag >> 8));
			}
		} else {
			hdr_len = 2;

			if (ptr != NULL) {
				/* Encode as TLV8 */
				if (buf_size < hdr_len + len) {
					KSI_pushError(tlv->ctx, res = KSI_BUFFER_OVERFLOW, NULL);
					goto cleanup;
				}
				*ptr-- = len & 0xff;
				*ptr-- = (unsigned char) (0x00 | (tlv->isNonCritical ? KSI_TLV_MASK_LENIENT : 0) | (tlv->isForwardable ? KSI_TLV_MASK_FORWARD : 0) | tlv->tag);

			}
		}
	}

	*buf_len = len + hdr_len;

	res = KSI_OK;

cleanup:

	return res;
}

int KSI_TLV_writeBytes(const KSI_TLV *tlv, unsigned char *buf, size_t buf_size, size_t *buf_len, int opt) {
	int res = KSI_UNKNOWN_ERROR;
	unsigned char *ptr = NULL;
	size_t i;
	size_t len;

	if (tlv == NULL || buf_len == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	res = serializeTlv(tlv, buf, buf_size, &len, opt);
	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}

	if ((opt & KSI_TLV_OPT_NO_MOVE) == 0 && buf != NULL) {
		/* Move the serialized value to the begin of the buffer. */
		ptr = buf;
		for (i = 0; i < len; i++) {
			*ptr++ = *(buf + buf_size - len + i);
		}
	}

	*buf_len = len;

	res = KSI_OK;

cleanup:

	KSI_nofree(ptr);

	return res;
}

int KSI_TLV_serialize_ex(const KSI_TLV *tlv, unsigned char *buf, size_t buf_size, size_t *len) {
	int res = KSI_UNKNOWN_ERROR;

	res = KSI_TLV_writeBytes(tlv, buf, buf_size, len, 0);
	if (res != KSI_OK) goto cleanup;

cleanup:

	return res;
}

int KSI_TLV_serialize(const KSI_TLV *tlv, unsigned char **buf, size_t *buf_len) {
	int res = KSI_UNKNOWN_ERROR;
	size_t tmp_len;

	unsigned char *tmp = NULL;

	tmp = KSI_calloc(4 + KSI_BUFFER_SIZE, 1);
	if (tmp == NULL) {
		res = KSI_OUT_OF_MEMORY;
		goto cleanup;
	}

	res = KSI_TLV_serialize_ex(tlv, tmp, 4 + KSI_BUFFER_SIZE, &tmp_len);
	if (res != KSI_OK) goto cleanup;


	*buf = tmp;
	*buf_len = tmp_len;

	tmp = NULL;

cleanup:

	KSI_free(tmp);

	return res;
}

/**
 *
 */
int KSI_TLV_serializePayload(KSI_TLV *tlv, unsigned char *buf, size_t *len) {
	return KSI_TLV_writeBytes(tlv, buf, *len, len, KSI_TLV_OPT_NO_HEADER);
}

#define NOTNEG(a) (a) < 0 ? 0 : a

static int stringify(const KSI_TLV *tlv, int indent, char *str, size_t size, size_t *len) {
	int res = KSI_UNKNOWN_ERROR;
	size_t l = *len;
	size_t i;

	if (*len >= size) {
		res = KSI_OK; /* Buffer is full, but do not break the flow. */
		goto cleanup;
	}
	if (indent != 0) {
		l += (size_t) KSI_snprintf(str + l, NOTNEG(size - l), "\n%*s", indent, "");
	}
	if (tlv->tag > 0xff) {
		l += (size_t) KSI_snprintf(str + l, NOTNEG(size - l), "TLV[0x%04x]", tlv->tag);
	} else {
		l += (size_t) KSI_snprintf(str + l, NOTNEG(size - l), "TLV[0x%02x]", tlv->tag);
	}

	l += (size_t) KSI_snprintf(str + l, NOTNEG(size - l), " %c", tlv->isNonCritical ? 'L' : '-');
	l += (size_t) KSI_snprintf(str + l, NOTNEG(size - l), " %c", tlv->isForwardable ? 'F' : '-');

	switch (tlv->payloadType) {
		case KSI_TLV_PAYLOAD_RAW:
			l += (size_t) KSI_snprintf(str + l, NOTNEG(size - l), " len = %llu : ", (unsigned long long)tlv->datap_len);
			for (i = 0; i < tlv->datap_len; i++) {
				l += (size_t) KSI_snprintf(str + l, NOTNEG(size - l), "%02x", tlv->datap[i]);
			}
			break;
		case KSI_TLV_PAYLOAD_TLV:
			l += (size_t) KSI_snprintf(str + l, NOTNEG(size - l), ":");
			for (i = 0; i < KSI_TLVList_length(tlv->nested); i++) {
				KSI_TLV *tmp = NULL;

				res = KSI_TLVList_elementAt(tlv->nested, i, &tmp);
				if (res != KSI_OK) goto cleanup;
				if (tmp == NULL) break;
				res = stringify(tmp, indent + 2, str, size, &l);
				if (res != KSI_OK) goto cleanup;
			}

			break;
		default:
			res = KSI_INVALID_ARGUMENT;
			goto cleanup;
	}

	if (l < size) {
		*len = l;
	} else {
		*len = size;
	}
	res = KSI_OK;

cleanup:

	return res;
}

char *KSI_TLV_toString(const KSI_TLV *tlv, char *buffer, size_t buffer_len) {
	int res = KSI_UNKNOWN_ERROR;
	char *ret = NULL;
	size_t tmp_len = 0;

	if (tlv == NULL || buffer == NULL) {
		goto cleanup;
	}

	res = stringify(tlv, 0, buffer, buffer_len, &tmp_len);
	if (res != KSI_OK) goto cleanup;

	ret = buffer;

cleanup:

	return ret;
}

static int expandNested(const KSI_TLV *sample, KSI_TLV *tlv) {
	int res = KSI_UNKNOWN_ERROR;
	size_t i;

	if (sample == NULL || tlv == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(sample->ctx);

	/* Fail if the TLV tags differ */
	if (sample->tag != tlv->tag) {
		KSI_pushError(sample->ctx, res = KSI_INVALID_ARGUMENT, "TLV types differ");
		goto cleanup;
	}

	/* Cast if necessary. */
	if (sample->payloadType != tlv->payloadType) {
		res = KSI_TLV_cast(tlv, sample->payloadType);
		KSI_pushError(sample->ctx, res, NULL);
		goto cleanup;
	}

	/* Continue if nested. */
	if (sample->payloadType == KSI_TLV_PAYLOAD_TLV) {
		/* Check if nested element count matches */
		if (KSI_TLVList_length(sample->nested) != KSI_TLVList_length(tlv->nested)) {
			KSI_pushError(sample->ctx, res = KSI_INVALID_ARGUMENT, "Different number of nested TLV's.");
			goto cleanup;
		}

		for (i = 0; i < KSI_TLVList_length(sample->nested); i++) {
			const KSI_TLV *nestedSample = NULL;
			KSI_TLV *nestedTlv = NULL;

			res = KSI_TLVList_elementAt(sample->nested, i, (KSI_TLV **)&nestedSample);
			if (res != KSI_OK) {
				KSI_pushError(sample->ctx, res, NULL);
				goto cleanup;
			}

			res = KSI_TLVList_elementAt(tlv->nested, i, &nestedTlv);
			if (res != KSI_OK) {
				KSI_pushError(sample->ctx, res, NULL);
				goto cleanup;
			}

			res = expandNested(nestedSample, nestedTlv);
			if (res != KSI_OK) {
				KSI_pushError(sample->ctx, res, NULL);
				goto cleanup;
			}

			/* The values are still components of the tlvs, so nothing has to be freed. */
			KSI_nofree(nestedTlv);
			KSI_nofree(nestedSample);
		}

	}

	res = KSI_OK;

cleanup:

	return res;
}

int KSI_TLV_clone(const KSI_TLV *tlv, KSI_TLV **clone) {
	int res = KSI_UNKNOWN_ERROR;
	unsigned char *buf = NULL;
	size_t buf_len;
	KSI_TLV *tmp = NULL;

	if (tlv == NULL || clone == NULL) {
		res = KSI_INVALID_ARGUMENT;
		goto cleanup;
	}

	KSI_ERR_clearErrors(tlv->ctx);

	/* Serialize the entire tlv */
	res = KSI_TLV_serialize(tlv, &buf, &buf_len);
	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}

	/* Recreate the TLV */
	res = KSI_TLV_parseBlob2(tlv->ctx, buf, buf_len, 1, &tmp);
	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}
	buf = NULL;

	/* Reexpand the nested (if any) TLV's */
	res = expandNested(tlv, tmp);
	if (res != KSI_OK) {
		KSI_pushError(tlv->ctx, res, NULL);
		goto cleanup;
	}

	*clone = tmp;
	tmp = NULL;

	res = KSI_OK;

cleanup:

	KSI_free(buf);
	KSI_TLV_free(tmp);

	return res;
}

size_t KSI_TLV_getAbsoluteOffset(const KSI_TLV *tlv) {
	return tlv->absoluteOffset;
}

size_t KSI_TLV_getRelativeOffset(const KSI_TLV *tlv) {
	return tlv->relativeOffset;
}


KSI_IMPLEMENT_GET_CTX(KSI_TLV);
