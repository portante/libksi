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

#ifndef KSI_NET_TCP_H_
#define KSI_NET_TCP_H_

#include "net.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

	typedef struct KSI_TcpClient_st KSI_TcpClient;

	/**
	 * Creates a new TCP client.
	 * \param[in]	ctx			KSI context.
	 * \param[out]	client		Pointer to the receiving pointer.
	 * \return status code (#KSI_OK, when operation succeeded, otherwise an error code).
	 */
	int KSI_TcpClient_new(KSI_CTX *ctx, KSI_TcpClient **client);

	/**
	 * Initializes an existing TCP client.
	 * \param[in]	ctx			KSI context.
	 * \param[out]	client		Pointer to the receiving pointer.
	 * \return status code (#KSI_OK, when operation succeeded, otherwise an error code).
	 */
	int KSI_TcpClient_init(KSI_CTX *ctx, KSI_TcpClient *client);

	/**
	 * Cleanup method for #KSI_HttpClient.
	 * \param[in]	client	Pointer to the HTTP client.
	 */
	void KSI_TcpClient_free(KSI_TcpClient *client);


	/**
	 * Setter for the publications file URL. The value will be copied and thus
	 * can be freed after successful call.
	 * \param[in]	client		Pointer to the http client.
	 * \param[in]	val			Null-terminated URL.
	 * \return status code (#KSI_OK, when operation succeeded, otherwise an error code).
	 */
	int KSI_TcpClient_setPublicationUrl(KSI_TcpClient *client, const char *val);


	/**
	 * Setter for the tcp client extender parameters.
     * \param[in]	client		Pointer to tcp client.
     * \param[in]	host		Host name.
     * \param[in]	port		Port number.
     * \param[in]	user		User name.
     * \param[in]	key			HMAC shared secret.
     * \return status code (#KSI_OK, when operation succeeded, otherwise an error code).
     */
	int KSI_TcpClient_setExtender(KSI_TcpClient *client, const char *host, unsigned port, const char *user, const char *key);

	/**
	 * Setter for the tcp aggregator parameters.
     * \param[in]	client		Pointer to tcp client.
     * \param[in]	host		Host name.
     * \param[in]	port		Port number.
     * \param[in]	user		User name.
     * \param[in]	key			HMAC shared secret.
     * \return status code (#KSI_OK, when operation succeeded, otherwise an error code).
     */
	int KSI_TcpClient_setAggregator(KSI_TcpClient *client, const char *host, unsigned port, const char *user, const char *key);

	/**
	 * Setter for the read, write, timeout in seconds.
	 * \param[in]	client		Pointer to the tcp client.
	 * \param[in]	val			Timeout in seconds.
	 * \return status code (#KSI_OK, when operation succeeded, otherwise an error code).
	 */
	int KSI_TcpClient_setTransferTimeoutSeconds(KSI_TcpClient *client, int val);

#ifdef __cplusplus
}
#endif

#endif /* KSI_NET_CURL_H_ */