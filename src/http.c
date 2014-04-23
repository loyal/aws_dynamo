/*
 * Copyright (c) 2006-2014 Devicescape Software, Inc.
 * This file is part of aws_dynamo, a C library for AWS DynamoDB.
 *
 * aws_dynamo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * aws_dynamo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with aws_dynamo.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include "aws_dynamo_utils.h"

#include <openssl/sha.h>
#include <openssl/buffer.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <string.h>
#include <stdio.h>

#include "http.h"

#include <stdio.h>

#include <curl/curl.h>
#include <string.h>

#include "http.h"

#define DEBUG_HTTP 1

#define LOCATION	"Location:"
#define LOCATION_LEN	9

/**
 * http_new_buffer - allocate a new buffer of the specified size
 * @handle: HTTP library handle
 * @size: size of the data buffer
 * Returns: Pointer to the newly allocated buffer, NULL on error
 */
static struct http_buffer *http_new_buffer(size_t size);

/**
 * http_free_buffer - free a previously allocated buffer
 * @handle: HTTP library handle
 * @buf: pointer to buffer
 */
static void http_free_buffer(struct http_buffer *buf);

struct http_curl_handle {
       CURL *curl;
       struct http_buffer *buf;
       char agent[128];
};

/**
 * http_strerror - obtain user readable error string
 * @error: error code to look up
 * Returns: string version of error
 */
const char *http_strerror(int error)
{
	return curl_easy_strerror(error);
}

/**
 * _curl_easy_perform - call curl_easy_perform(), retry once
 * @curl: HTTP handle
 * Returns: HTTP_* result code (HTTP_OK, etc.)
 */
static int _curl_easy_perform(CURL *curl)
{
	int ret;

	ret = curl_easy_perform(curl);

	if (ret == CURLE_OPERATION_TIMEOUTED) /* (sic) */ {
		ret = curl_easy_perform(curl);
	}

	if (ret == CURLE_OK)
		return HTTP_OK;

#if DEBUG_HTTP
	Debug("HTTP ERROR: %s\n", curl_easy_strerror(ret));
#endif

	if (ret == CURLE_SSL_PEER_CERTIFICATE ||
	    ret == CURLE_SSL_CERTPROBLEM ||
	    ret == CURLE_SSL_CACERT
#if LIBCURL_VERSION_NUM >= 0x071000 /* Only if this version supports it */
	    || ret == CURLE_SSL_CACERT_BADFILE
#endif
	   ) {
		return HTTP_CERT_FAILURE;
	}

	return HTTP_FAILURE;
}

/**
 * http_transaction - issue an HTTP transaction to the specified URL
 * @handle: HTTP handle
 * @url: URL to post to
 * @data: data string to send with post
 * @con_close: close connection? (1=yes; 0=no)
 * @hdrs: a linked list of headers to be included in the request
 * Returns: HTTP_* result code (HTTP_OK, etc.)
 */
static int http_transaction(void *handle, const char *url,
			    const char *data, int con_close,
		   	    struct http_headers *hdrs)
{
	int ret;
	char *effective_url;
	struct http_curl_handle *h = handle;
	CURL *curl = h->curl;
	struct curl_slist *headers = NULL;
	struct http_buffer *buf = h->buf;

	Debug("aws_post: "__FILE__":%d",__LINE__);

	http_reset_buffer(buf);
	Debug("aws_post: "__FILE__":%d",__LINE__);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	Debug("aws_post: "__FILE__":%d",__LINE__);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_receive_data);
	Debug("aws_post: "__FILE__":%d",__LINE__);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
	Debug("aws_post: "__FILE__":%d",__LINE__);

	if (data) {
        Debug("aws_post: "__FILE__":%d",__LINE__);
		curl_easy_setopt(curl, CURLOPT_POST, 1);
        Debug("aws_post: "__FILE__":%d",__LINE__);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
	} else {
		/* Use HTTP GET */
        Debug("aws_post: "__FILE__":%d",__LINE__);
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
	}
	Debug("aws_post: "__FILE__":%d",__LINE__);

	if (con_close != HTTP_NOCLOSE)
		curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1);
	else
		curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0);

	if (hdrs) {
        Debug("aws_post: "__FILE__":%d",__LINE__);
		int i;

		for (i = 0; i < hdrs->count; i++) {
			char *header;
            Debug("aws_post: "__FILE__":%d",__LINE__);

			if (asprintf(&header, "%s: %s",
				     hdrs->entries[i].name,
				     hdrs->entries[i].value) == -1) {
				if (headers)
					curl_slist_free_all(headers);
				return HTTP_FAILURE;
			}
			headers = curl_slist_append(headers, header);
			free(header);
		}
	}
	Debug("aws_post: "__FILE__":%d",__LINE__);

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	Debug("aws_post: "__FILE__":%d",__LINE__);

	/* Perform the transfer */
	if ((ret = _curl_easy_perform(curl)) == 0) {
        Debug("aws_post: "__FILE__":%d",__LINE__);
		/* Get a copy of the response code */
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &buf->response);

		/* Get a copy of the effective URL */
        Debug("aws_post: "__FILE__":%d",__LINE__);
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
        Debug("aws_post: "__FILE__":%d",__LINE__);
        buf->url = strdup(effective_url);
	}

	if (hdrs) {	
        Debug("aws_post: "__FILE__":%d",__LINE__);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
        Debug("aws_post: "__FILE__":%d",__LINE__);
		curl_slist_free_all(headers);
	}
	Debug("aws_post: "__FILE__":%d",__LINE__);

	return ret;
}

/**
 * http_fetch_url - fetch a URL into the specified buffer
 * @handle: HTTP handle
 * @url: URL to fetch
 * @con_close: close connection? (1=yes; 0=no)
 * @headers: a list of headers to be included in the request
 * Returns: HTTP_* result code (HTTP_OK, etc.)
 */
int http_fetch_url(void *handle, const char *url,
		   int con_close,
		   struct http_headers *headers)
{
	return http_transaction(handle, url, NULL,
				con_close, headers);
}

/**
 * http_post - post a form back to the specified URL
 * @handle: HTTP handle
 * @url: URL to post to
 * @data: data string to send with post
 * @headers: a list of headers to be included in the request
 * Returns: HTTP_* result code (HTTP_OK, etc.)
 */
int http_post(void *handle, const char *url,
		   const char *data,
		   struct http_headers *headers)
{
	return http_transaction(handle, url, data,
				HTTP_NOCLOSE, headers);
}

/**
 * http_clear_cookies - clear any cookies stored by the http library
 * @handle: HTTP handle
 */
void http_clear_cookies(void *handle)
{
#if LIBCURL_VERSION_NUM >= 0x070e01 /* Only if this version supports it */
	struct http_curl_handle *h = handle;
	CURL *curl = h->curl;
	
	curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
#endif
}

/**
 * http_init - initialise an HTTP session
 * Returns: HTTP handle to use in other calls
 */
void *http_init()
{
	struct http_curl_handle *h;
	CURL *curl;

	if ((h = malloc(sizeof(*h))) == NULL)
		return NULL;

   /* Create page buffer */
   h->buf = http_new_buffer(HTTP_PAGE_BUFFER_SIZE);
   if (h->buf == NULL) {
      Warnx("Failed to allocate buffer for page\n");
		free(h);
		return NULL;
   }

	/* Create the agent string */
	snprintf(h->agent, sizeof(h->agent), "Devicescape-AWS-DynamoDB-C/0.1");

	if ((curl = curl_easy_init()) == NULL) {
      Warnx("Failed to init curl easy handle\n");
		http_free_buffer(h->buf);
		free(h);
		return NULL;
	}

	/* Set the maximum time in seconds that a transfer can take. */
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);
		
	/* Tell curl not to use signals for timeout's etc. */
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, h->agent);

	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2);

	h->curl = curl;

	return h;
}

/**
 * http_deinit - terminate an HTTP session
 * @handle: HTTP handle
 */
void http_deinit(void *handle)
{
	struct http_curl_handle *h = handle;

	curl_easy_cleanup(h->curl);
	http_free_buffer(h->buf);
	free(h);
}

/**
 * http_new_buffer - allocate a new buffer of the specified size
 * @size: size of the data buffer
 * Returns: Pointer to the newly allocated buffer, NULL on error
 */
static struct http_buffer *http_new_buffer(size_t size)
{
	struct http_buffer *buf;

	if ((buf = malloc(sizeof(*buf))) == NULL)
		return NULL;
	memset(buf, 0, sizeof(*buf));

	if ((buf->data = malloc(size)) == NULL) {
		free(buf);
		return NULL;
	}
	memset(buf->data, 0, size);

	buf->max = size;

	return buf;
}

/**
 * http_free_buffer - free a previously allocated buffer
 * @handle: HTTP handle
 * @buf: pointer to buffer
 */
static void http_free_buffer(struct http_buffer *buf)
{
	free(buf->data);
	free(buf->url);
	free(buf->base);
	free(buf);
}

/**
 * http_reset_buffer - reset the pointers in a buffer
 * @buf: buffer to reset
 */
void http_reset_buffer(struct http_buffer *buf)
{
	free(buf->url);
	buf->url = NULL;
	free(buf->base);
	buf->base = NULL;
	buf->cur = 0;
	memset(buf->data, 0, buf->max);
}

/**
 * http_get_data - return pointer to raw data buffer (read-only)
 * @handle: HTTP library handle
 * @buf: pointer to buffer structure
 * @len: length of buffer (out)
 * Returns: const pointer to raw data
 */
const unsigned char *http_get_data(void *handle, int *len)
{
	struct http_curl_handle *h = handle;
	struct http_buffer *buf = h->buf;

	if (len) {
		*len = buf->cur;
		return buf->data;
	}
	return NULL;
}

int http_get_response_code(void *handle)
{
	struct http_curl_handle *h = handle;
	struct http_buffer *buf = h->buf;

	return buf->response;
}

/**
 * http_get_url - get the current real URL from a buffer
 * @handle: HTTP library handle
 * @buf: pointer to buffer structure
 * Returns: read-only pointer to URL
 */
const char *http_get_url(void *handle)
{
	struct http_curl_handle *h = handle;
	struct http_buffer *buf = h->buf;
	return buf->url;
}

/**
 * http_receive_data - callback for processing data received over HTTP
 * @ptr: pointer to the current chunk of data
 * @size: size of each element
 * @nmemb: number of elements
 * @arg: buffer to store chunk in
 * Returns: amount of data processed
 */
size_t http_receive_data(void *ptr, size_t size, size_t nmemb, void *arg)
{
	size_t len = size * nmemb;
	struct http_buffer *buf = arg;

	if (len > (size_t)(buf->max - buf->cur)) {
		/* Too much data for buffer */
		len = buf->max - buf->cur;
		Warnx("Only storing %zd bytes; buffer too small\n", len);
	}
	memcpy(buf->data + buf->cur, ptr, len);
	buf->cur += len;

	return len;
}

/**
 * _http_fetch_url_quiet - fetch a URL into the specified buffer, 
 * 	internal function, does not print debug info
 * @handle: HTTP library handle
 * @url: URL to fetch
 * @con_close: close the HTTP connection? (1=yes; 0=no)
 * @hdrs: a list of headers to be included in the request or NULL
 * Returns: HTTP_* result code (HTTP_OK, etc.)
 */
int _http_fetch_url_quiet(void *handle, const char *url,
		    int con_close,
		    struct http_headers *hdrs)
{
	struct http_curl_handle *h = handle;
	struct http_buffer *buf = h->buf;
	int rv;
	struct http_headers headers;
	int num_headers;
	int i = 0;

	num_headers = (hdrs ? hdrs->count : 0);

	headers.count = num_headers;
	headers.entries = calloc(num_headers, sizeof(headers.entries[0]));
	if (headers.entries == NULL)
		return HTTP_FAILURE;

	if (hdrs) {
		for (; i < (int)(hdrs->count); i++) {
			headers.entries[i].name = hdrs->entries[i].name;
			headers.entries[i].value = hdrs->entries[i].value;
		}
	}

	rv = http_fetch_url(handle, url, con_close,
			    &headers);

	if (buf->cur >= buf->max)
		buf->cur = buf->max - 1;
	buf->data[buf->cur] = '\0';

	free(headers.entries);

	return rv;
}

/**
 * _http_fetch_url - fetch a URL into the specified buffer, internal function
 * @handle: HTTP library handle
 * @url: URL to fetch
 * @con_close: close the HTTP connection? (1=yes; 0=no)
 * @hdrs: a list of headers to be included in the request or NULL
 * Returns: HTTP_* result code (HTTP_OK, etc.)
 */
int _http_fetch_url(void *handle, const char *url,
		    int con_close,
		    struct http_headers *hdrs)
{
	int rv;
#if DEBUG_HTTP
	struct http_curl_handle *h = handle;
	struct http_buffer *buf = h->buf;
	int i;

	Debug("HTTP GET: %s (%s)\n", url,
		  con_close ? "CLOSE" : "NO_CLOSE");

	if (hdrs) {
		for (i = 0 ; i < hdrs->count; i++) {
			Debug("HTTP HEADER: %s: %s\n", hdrs->entries[i].name, hdrs->entries[i].value);
		}
	}
#endif

	rv = _http_fetch_url_quiet(handle, url, con_close,
				   hdrs);

#if DEBUG_HTTP
	Debug("HTTP RECV %d BYTES:\n%s\n", buf->cur, buf->data);

	if (rv == HTTP_CERT_FAILURE)
		Debug("HTTP ERROR: certificate problem\n");
#endif

	return rv;
}

/**
 * _http_post_quiet - post a form back to the specified URL,
 * 	internal function, does not print debug info
 * @handle: HTTP handle
 * @url: URL to post to
 * @data: data string to send with post
 * @headers: a list of headers to be included in the request
 * Returns: HTTP_* result code (HTTP_OK, etc.)
 */
int _http_post_quiet(void *handle, const char *url,
		    const char *data,
		   struct http_headers *headers)
{
	struct http_curl_handle *h = handle;
	struct http_buffer *buf = h->buf;
	int rv;
       
	rv = http_post(handle, url, data, headers);

	if (buf->cur >= buf->max)
		buf->cur = buf->max - 1;
	buf->data[buf->cur] = '\0';

	return rv;
}

/**
 * _http_post - post a form back to the specified URL, internal function
 * @handle: HTTP handle
 * @url: URL to post to
 * @data: data string to send with post
 * @headers: a list of headers to be included in the request
 * Returns: HTTP_* result code (HTTP_OK, etc.)
 */
int _http_post(void *handle, const char *url,
		    const char *data,
		   struct http_headers *hdrs)
{
	struct http_curl_handle *h = handle;
	struct http_buffer *buf = h->buf;
	int rv;
#if DEBUG_HTTP
	int i;

	Debug("HTTP POST: %s", url);

	if (hdrs) {
		for (i = 0 ; i < hdrs->count; i++) {
			Debug("HTTP HEADER: %s: %s", hdrs->entries[i].name, hdrs->entries[i].value);
		}
	}
#endif
	Debug("aws_post: "__FILE__":%d",__LINE__);

	rv = _http_post_quiet(handle, url, data, hdrs);
	Debug("aws_post: "__FILE__":%d",__LINE__);

	if (buf->cur >= buf->max)
		buf->cur = buf->max - 1;
	buf->data[buf->cur] = '\0';

	Debug("aws_post: "__FILE__":%d",__LINE__);

#if DEBUG_HTTP
	Debug("HTTP RECV %d BYTES:\n%s\n", buf->cur, buf->data);

	if (rv == HTTP_CERT_FAILURE)
		Debug("HTTP ERROR: certificate problem\n");
#endif

	return rv;
}

/**
 * http_url_encode - URL encode a string
 * @str: NULL-terminated string to encode
 * Returns: allocated string URL encoded for AWS 
 */
char *http_url_encode(void *handle, const char *string)
{
	struct http_curl_handle *h = handle;
	char *curl_encoded;
	char *encoded;

	curl_encoded = curl_easy_escape(h->curl, string, strlen(string));
	if (curl_encoded == NULL) {
		return NULL;
	}

	encoded = strdup(curl_encoded);
	curl_free(curl_encoded);
	return encoded;
}

int http_set_https_certificate_file(void *handle, const char *filename)
{
	struct http_curl_handle *h = handle;

	curl_easy_setopt(h->curl, CURLOPT_CAINFO, filename);
	return 0;
}
