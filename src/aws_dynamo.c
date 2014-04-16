/*
 * Copyright (c) 2012-2014 Devicescape Software, Inc.
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

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <yajl/yajl_parse.h>

#include "http.h"
#include "aws.h"
#include "aws_iam.h"
#include "aws_sigv4.h"
#include "aws_dynamo.h"
#include "aws_dynamo_query.h"

//#define DEBUG_AWS_DYNAMO 1
//#define DEBUG_PARSER 1

const char *aws_dynamo_attribute_types[] = {
	AWS_DYNAMO_JSON_TYPE_STRING,
	AWS_DYNAMO_JSON_TYPE_STRING_SET,
	AWS_DYNAMO_JSON_TYPE_NUMBER,
	AWS_DYNAMO_JSON_TYPE_NUMBER_SET,
};

struct aws_errors aws_kinesis_errors[] = {
    { .error="Unknown", .reason="Unknown", .http_code=-1 },
    { .error="IncompleteSignature", .reason="The request signature does not conform to AWS standards.", .http_code=400 },
    { .error="InternalFailure", .reason="The request processing has failed because of an unknown error, exception or failure.", .http_code=500 },
    { .error="InvalidAction", .reason="The action or operation requested is invalid. Verify that the action is typed correctly.", .http_code=400 },
    { .error="InvalidClientTokenId", .reason="The X.509 certificate or AWS access key ID provided does not exist in our records.", .http_code=403 },
    { .error="InvalidParameterCombination", .reason="Parameters that must not be used together were used together.", .http_code=400 },
    { .error="InvalidParameterValue", .reason="An invalid or out-of-range value was supplied for the input parameter.", .http_code=400 },
    { .error="InvalidQueryParameter", .reason="The AWS query string is malformed or does not adhere to AWS standards.", .http_code=400 },
    { .error="MalformedQueryString", .reason="The query string contains a syntax error.", .http_code=404 },
    { .error="MissingAction", .reason="The request is missing an action or a required parameter.", .http_code=400 },
    { .error="MissingAuthenticationToken", .reason="The request must contain either a valid (registered) AWS access key ID or X.509 certificate.", .http_code=403 },
    { .error="MissingParameter", .reason="A required parameter for the specified action is not supplied.", .http_code=400 },
    { .error="OptInRequired", .reason="The AWS access key ID needs a subscription for the service.", .http_code=403 },
    { .error="RequestExpired", .reason="The request reached the service more than 15 minutes after the date stamp on the request or more than 15 minutes after the request expiration date (such as for pre-signed URLs), or the date stamp on the request is more than 15 minutes in the future.", .http_code=400 },
    { .error="ServiceUnavailable", .reason="The request has failed due to a temporary failure of the server.", .http_code=503 },
    { .error="Throttling", .reason="The request was denied due to request throttling.", .http_code=400 },
    { .error="ValidationError", .reason="The input fails to satisfy the constraints specified by an AWS service.", .http_code=400 },
};

static char *aws_dynamo_get_canonicalized_headers(struct http_headers *headers) {
    int i;
	int canonical_headers_len = 0;
	char *canonical_headers;
	char *ptr;

	/* Assume all header .name fields are lowercase. */
    
	/* Assume no duplicate headers (that is, all header names are
		distinct.) */

	/* Assume headers are already sorted alphabetically by header name. */

	for (i = 0; i < headers->count; i++) {
		canonical_headers_len += strlen(headers->entries[i].name);
		canonical_headers_len += strlen(headers->entries[i].value);
		canonical_headers_len += 2; /* ':' and '\n' */
	}
	canonical_headers_len++; /* \0 terminator */

	ptr = canonical_headers = calloc(sizeof(char), canonical_headers_len);

	if (ptr == NULL) {
		Err("aws_dynamo_get_canonicalized_headers: Failed to allocate.");
		return NULL;
	}

	for (i = 0; i < headers->count; i++) {
		int n;
		int remaining;

		remaining = canonical_headers_len - (ptr - canonical_headers);

		n = snprintf(ptr, remaining, "%s:%s\n", headers->entries[i].name,
			headers->entries[i].value);

		if (n == -1 || n >= remaining) {
			Warnx("aws_dynamo_get_canonicalized_headers: string buffer not large enough.");
			free(canonical_headers);
			return NULL;
		}
		ptr += n;
	}

	return canonical_headers;
}

/*
1. rename this fuction to aws_post(), give it a new argument "aws_service"
Loyal will call it like this:

aws_post(aws, "kinesis", "Kinesis_20131202.PutRecord", "{json body...}");


*/

static int aws_post(struct aws_handle *aws, const char *aws_service, const char *target, const char *body) {
	char iso8601_basic_date[128];
	char host_header[256];
	char authorization[256];
	struct http_header hdrs[] = {
		/* Note: The .name fields must all lowercase and the headers included
			in the signature must be sorted here.  This simplifies the signature
			calculation. */
		{ .name = HTTP_HOST_HEADER, .value = host_header },
		{ .name = AWS_DYNAMO_DATE_HEADER, .value = iso8601_basic_date },
		{ .name = AWS_DYNAMO_TARGET_HEADER, .value = target },
		/* begin headers not included in signature. */
		{ .name = AWS_DYNAMO_AUTHORIZATION_HEADER, .value = authorization },
		{ .name = HTTP_CONTENT_TYPE_HEADER, .value = AWS_DYNAMO_CONTENT_TYPE },
	};
	struct http_headers headers = {
		.count = 3,
		/* AWS_DYNAMO_AUTHORIZATION and HTTP_CONTENT_TYPE_HEADER are
		   not included for now since they are not used in the
		   signature calculation. */
		.entries = hdrs,
	};
	const char *signed_headers = HTTP_HOST_HEADER ";" AWS_DYNAMO_DATE_HEADER ";" AWS_DYNAMO_TARGET_HEADER;
	struct tm tm;
	time_t now;
	char *canonical_headers = NULL;
	char *hashed_canonical_request = NULL;
	char *string_to_sign = NULL;
	char *signature = NULL;
	int n;
	char *url = NULL;
	const char *scheme;
	const char *host;
	const char *aws_secret_access_key;
	const char *aws_access_key_id;
	char yyyy_mm_dd[16];

    /* FIXME - choose host based on service enum, no strcmp. */
	if (strcmp(aws_service, "dynamodb") == 0) {
        if (aws->dynamo_host) {
            host = AWS_KINESIS_DEFAULT_HOST;
        } else {
            host = AWS_DYNAMO_DEFAULT_HOST;
        }
	} else if (strcmp(aws_service, "kinesis") == 0) {
        host = "kinesis.us-east-1.amazonaws.com";
	} else {
        Warnx("aws_post: Bad service");
    }

	n = snprintf(host_header, sizeof(host_header), "%s", host);

	if (n == -1 || n >= sizeof(host_header)) {
		Warnx("aws_post: host header truncated");
		goto failure;
	}

	if (time(&now) == -1) {
		Warnx("aws_post: Failed to get time.");
		return -1;
	}

	if (aws->aws_id == NULL && aws->aws_key == NULL &&
	    aws->token->expiration - now <= AWS_SESSION_REFRESH_TIME) {
		struct aws_session_token *new_token;

		new_token = aws_iam_load_default_token(aws);

		if (new_token == NULL) {
			Warnx("aws_post: Failed to refresh token.");
		} else {
			struct aws_session_token *old_token;
			old_token = aws->token;
			aws->token = new_token;
			aws_free_session_token(old_token);   
		}

		aws_secret_access_key = aws->token->secret_access_key;
		aws_access_key_id = aws->token->access_key_id;
	} else {
		aws_secret_access_key = aws->aws_key;
		aws_access_key_id = aws->aws_id;
	}

	if (gmtime_r(&now, &tm) == NULL) {
		Warnx("aws_post: Failed to get time structure.");
		return -1;
	}

	if (strftime(iso8601_basic_date, sizeof(iso8601_basic_date),
		     "%Y%m%dT%H%M%SZ", &tm) == 0) {
		Warnx("aws_post: Failed to format time.");
		return -1;
	}

	if (strftime(yyyy_mm_dd, sizeof(yyyy_mm_dd), "%Y%m%d", &tm) == 0) {
		Warnx("aws_post: Failed to format date.");
		return -1;
	}

	canonical_headers = aws_dynamo_get_canonicalized_headers(&headers);

	if (canonical_headers == NULL) {
		Warnx("aws_post: Failed to get canonical_headers.");
		return -1;
	}

	hashed_canonical_request = aws_sigv4_create_hashed_canonical_request("POST", "/", "",
		canonical_headers, signed_headers, body);

	if (hashed_canonical_request == NULL) {
		Warnx("aws_post: Failed to get canonical request.");
		goto failure;
	}

	string_to_sign = aws_sigv4_create_string_to_sign(
		iso8601_basic_date, yyyy_mm_dd,
		"us-east-1" /* FIXME - hard coded region. */,
		aws_service, 
		hashed_canonical_request);
	if (string_to_sign == NULL) {
		Warnx("aws_post: Failed to get string to sign.");
		goto failure;
	}

	signature = aws_sigv4_create_signature(aws_secret_access_key,
		yyyy_mm_dd, 
		"us-east-1" /* FIXME - hard coded region. */,
		aws_service,
		string_to_sign);

	if (signature == NULL) {
		Warnx("aws_post: Failed to get signature.");
		goto failure;
	}

	n = snprintf(authorization, sizeof(authorization),
                 /* FIXME - hard coded region */
                 "AWS4-HMAC-SHA256 Credential=%s/%s/us-east-1/%s/aws4_request,SignedHeaders=%s,Signature=%s", 
                 aws_access_key_id, yyyy_mm_dd, aws_service, signed_headers, signature);

	if (n == -1 || n >= sizeof(authorization)) {
		Warnx("aws_post: authorization truncated");
		goto failure;
	}

	/* Include all headers now that the signature calculation is complete. */
	headers.count = sizeof(hdrs) / sizeof(hdrs[0]);

#ifdef DEBUG_AWS_DYNAMO
	Debug("aws_post: '%s'", body);
#endif
        /* FIXME: make https/http not DynamoDB specific, just have 1 
           boolean that controls for the whole library if we use https or not. */
	if (aws->dynamo_https) {
		scheme = "https";
	} else {
		scheme = "http";
	}

        /* FIXME: make the kinesis service port configurable?  Or not? */
	if (aws->dynamo_port > 0) {
		if (asprintf(&url, "%s://%s:%d/", scheme, host, aws->dynamo_port) == -1) {
			Warnx("aws_post: failed to create url");
			goto failure;

		}
	} else {
		if (asprintf(&url, "%s://%s/", scheme, host) == -1) {
			Warnx("aws_post: failed to create url");
			goto failure;
		}
	}

	if (_http_post(aws->http, url, body, &headers) != HTTP_OK) {
		Warnx("aws_post: HTTP post failed, will retry.");
		usleep(100000);
		if (_http_post(aws->http, url, body, &headers) != HTTP_OK) {
			Warnx("aws_post: Retry failed.");
			goto failure;
		}
	}

#ifdef DEBUG_AWS_DYNAMO
	{
		int response_len;

		Debug("aws_post response: '%s'", http_get_data(aws->http, &response_len));
	}
#endif

	free(canonical_headers);
	free(hashed_canonical_request);
	free(string_to_sign);
	free(signature);
	free(url);

	return 0;
failure:
	free(canonical_headers);
	free(hashed_canonical_request);
	free(string_to_sign);
	free(signature);
	free(url);

	return -1;
}

static int aws_dynamo_post(struct aws_handle *aws, const char *target, const char *body) {
  return aws_post(aws, "dynamodb", target, body);
}

enum {
	ERROR_RESPONSE_PARSER_STATE_NONE,
	ERROR_RESPONSE_PARSER_STATE_ROOT_MAP,
	ERROR_RESPONSE_PARSER_STATE_TYPE_KEY,
	ERROR_RESPONSE_PARSER_STATE_MESSAGE_KEY,
};

struct error_response_parser_ctx {
	int code;
	char *message;
	int parser_state;
};

static int error_response_parser_string(void *ctx, const unsigned char *val,
				 unsigned int len)
{
	struct error_response_parser_ctx *_ctx = (struct error_response_parser_ctx *)ctx;
#ifdef DEBUG_PARSER
	char buf[len + 1];
	snprintf(buf, len + 1, "%s", val);

	Debug("error_response_parser_string, val = %s, enter state %d", buf,
	      _ctx->parser_state);
#endif				/* DEBUG_PARSER */


	switch (_ctx->parser_state) {
	case ERROR_RESPONSE_PARSER_STATE_TYPE_KEY: {
		const unsigned char *type;
		int typelen;

		type = memchr(val, '#', len);
		if (type == NULL) {
			Warnx("error_response_parser_string - # not found");
			return 0;	
		}
		type++;

		typelen = len - (type - val);

		if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_ACCESS_DENIED_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_ACCESS_DENIED_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_CONDITIONAL_CHECK_FAILED_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_CONDITIONAL_CHECK_FAILED_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_INCOMPLETE_SIGNATURE_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_INCOMPLETE_SIGNATURE_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_LIMIT_EXCEEDED_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_LIMIT_EXCEEDED_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_MISSING_AUTHENTICATION_TOKEN_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_MISSING_AUTHENTICATION_TOKEN_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_PROVISIONED_THROUGHPUT_EXCEEDED_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_PROVISIONED_THROUGHPUT_EXCEEDED_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_RESOURCE_IN_USE_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_RESOURCE_IN_USE_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_RESOURCE_NOT_FOUND_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_RESOURCE_NOT_FOUND_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_THROTTLING_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_THROTTLING_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_VALIDATION_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_VALIDATION_EXCEPTION;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_INTERNAL_FAILURE, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_INTERNAL_FAILURE;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_INTERNAL_SERVER_ERROR, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_INTERNAL_SERVER_ERROR;
		} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_SERVICE_UNAVAILABLE_EXCEPTION, type, typelen)) {
			_ctx->code = AWS_DYNAMO_CODE_SERVICE_UNAVAILABLE_EXCEPTION;
		} else {
			char code[typelen + 1];
			snprintf(code, typelen + 1, "%s", type);
			Warnx("error_response_parser_string - unknown code %s", code);
		}
		_ctx->parser_state = ERROR_RESPONSE_PARSER_STATE_ROOT_MAP;
		break;
	}
	case ERROR_RESPONSE_PARSER_STATE_MESSAGE_KEY: {
		free(_ctx->message);
		_ctx->message = strndup(val, len);
		_ctx->parser_state = ERROR_RESPONSE_PARSER_STATE_ROOT_MAP;
		break;
	}
	default:{
			Warnx("error_response_parser_string - unexpected state %d", _ctx->parser_state);
			return 0;
			break;
		}
	}

#ifdef DEBUG_PARSER
	Debug("error_response_parser_string exit %d", _ctx->parser_state);
#endif				/* DEBUG_PARSER */

	return 1;
}

static int error_response_parser_start_map(void *ctx)
{
	struct error_response_parser_ctx *_ctx = (struct error_response_parser_ctx *)ctx;

#ifdef DEBUG_PARSER
	Debug("error_response_parser_start_map, enter state %d", _ctx->parser_state);
#endif				/* DEBUG_PARSER */

	switch (_ctx->parser_state) {
	case ERROR_RESPONSE_PARSER_STATE_NONE: {
		_ctx->parser_state = ERROR_RESPONSE_PARSER_STATE_ROOT_MAP;
		break;
	}
	default:{
			Warnx("error_response_parser_start_map - unexpected state: %d",
			     _ctx->parser_state);
			return 0;
			break;
		}
	}

#ifdef DEBUG_PARSER
	Debug("error_response_parser_start_map exit %d", _ctx->parser_state);
#endif				/* DEBUG_PARSER */
	return 1;
}

static int error_response_parser_map_key(void *ctx, const unsigned char *val,
				  unsigned int len)
{
	struct error_response_parser_ctx *_ctx = (struct error_response_parser_ctx *)ctx;
#ifdef DEBUG_PARSER
	char buf[len + 1];
	snprintf(buf, len + 1, "%s", val);

	Debug("error_response_parser_map_key, val = %s, enter state %d", buf,
	      _ctx->parser_state);
#endif				/* DEBUG_PARSER */

	switch (_ctx->parser_state) {
	case ERROR_RESPONSE_PARSER_STATE_ROOT_MAP: {
			if (AWS_DYNAMO_VALCMP("__type", val, len)) {
				_ctx->parser_state = ERROR_RESPONSE_PARSER_STATE_TYPE_KEY;
			} else if (AWS_DYNAMO_VALCASECMP("message", val, len)) {
				_ctx->parser_state = ERROR_RESPONSE_PARSER_STATE_MESSAGE_KEY;
			}
		break;
	}
	default:{
			Warnx("error_response_parser_map_key - unexpected state %d", _ctx->parser_state);
			return 0;
			break;
		}
	}

#ifdef DEBUG_PARSER
	Debug("error_response_parser_map_key exit %d", _ctx->parser_state);
#endif				/* DEBUG_PARSER */
	return 1;
}

static int error_response_parser_end_map(void *ctx)
{
	struct error_response_parser_ctx *_ctx = (struct error_response_parser_ctx *)ctx;
#ifdef DEBUG_PARSER
	Debug("error_response_parser_end_map enter %d", _ctx->parser_state);
#endif				/* DEBUG_PARSER */

	switch (_ctx->parser_state) {
	case ERROR_RESPONSE_PARSER_STATE_ROOT_MAP:{
			_ctx->parser_state = ERROR_RESPONSE_PARSER_STATE_NONE;
			break;
		}
	default:{
			Warnx("error_response_parser_end_map - unexpected state %d", _ctx->parser_state);
			return 0;
			break;
		}
	}

#ifdef DEBUG_PARSER
	Debug("error_response_parser_end_map exit %d", _ctx->parser_state);
#endif				/* DEBUG_PARSER */

	return 1;
}

static yajl_callbacks error_response_parser_callbacks = {
	.yajl_string = error_response_parser_string,
	.yajl_start_map = error_response_parser_start_map,
	.yajl_map_key = error_response_parser_map_key,
	.yajl_end_map = error_response_parser_end_map,
};

/* returns 1 if the request should be retried, 0 if the request shouldn't be
	retried, -1 on error. */
static int aws_dynamo_parse_error_response(const unsigned char *response, int response_len, char **message, int *code)
{
	yajl_handle hand;
	yajl_status stat;
	struct error_response_parser_ctx _ctx = {
		.code = AWS_DYNAMO_CODE_UNKNOWN,
	};
	int rv;

#if YAJL_MAJOR == 2
	hand = yajl_alloc(&error_response_parser_callbacks, NULL, &_ctx);
	yajl_parse(hand, response, response_len);
	stat = yajl_complete_parse(hand);
#else
	hand = yajl_alloc(&error_response_parser_callbacks, NULL, NULL, &_ctx);
	yajl_parse(hand, response, response_len);
	stat = yajl_parse_complete(hand);
#endif

	if (stat != yajl_status_ok) {
		unsigned char *str =
		    yajl_get_error(hand, 1, response, response_len);
		Warnx("aws_dynamo_parse_error_response: json parse failed, '%s'", (const char *)str);
		yajl_free_error(hand, str);
		yajl_free(hand);
		free(_ctx.message);
		*code = AWS_DYNAMO_CODE_UNKNOWN;
		return -1;
	}

	if (_ctx.message != NULL) {
		free(*message);
		*message = strdup(_ctx.message);
		free(_ctx.message);
		*code = _ctx.code;
	}

	switch(_ctx.code) {
	case AWS_DYNAMO_CODE_ACCESS_DENIED_EXCEPTION:
	case AWS_DYNAMO_CODE_CONDITIONAL_CHECK_FAILED_EXCEPTION:
	case AWS_DYNAMO_CODE_INCOMPLETE_SIGNATURE_EXCEPTION:
	case AWS_DYNAMO_CODE_LIMIT_EXCEEDED_EXCEPTION:
	case AWS_DYNAMO_CODE_MISSING_AUTHENTICATION_TOKEN_EXCEPTION:
	case AWS_DYNAMO_CODE_RESOURCE_IN_USE_EXCEPTION:
	case AWS_DYNAMO_CODE_RESOURCE_NOT_FOUND_EXCEPTION:
	case AWS_DYNAMO_CODE_VALIDATION_EXCEPTION:
		/* the request should not be retried. */
		rv = 0;
		break;

	case AWS_DYNAMO_CODE_PROVISIONED_THROUGHPUT_EXCEEDED_EXCEPTION:
	case AWS_DYNAMO_CODE_THROTTLING_EXCEPTION:
	case AWS_DYNAMO_CODE_INTERNAL_FAILURE:
	case AWS_DYNAMO_CODE_INTERNAL_SERVER_ERROR:
	case AWS_DYNAMO_CODE_SERVICE_UNAVAILABLE_EXCEPTION:
		/* the request should be retried. */
		rv = 1;
		break;

	case AWS_DYNAMO_CODE_UNKNOWN:
	default:
		rv = -1;
		break;
	}

	yajl_free(hand);
	return rv;
}

int aws_kinesis_request(struct aws_handle *aws, const char *target, const char *body) {
	int http_response_code;
	int dynamodb_response_code = AWS_KINESIS_CODE_UNKNOWN;
	int rv = -1;
	int attempt = 0;
	char *message = NULL;

	do {

		if (aws_post(aws, "kinesis", target, body) == -1) {
			return -1;
		}
	
		http_response_code = http_get_response_code(aws->http);

		if (http_response_code == 200) {
			rv = 0;
			break;
		} else {
			const char *response;
			int response_len;
			int retry;
			useconds_t backoff;

			response = http_get_data(aws->http, &response_len);

			if (response == NULL) {
				Warnx("aws_dynamo_request: Failed to get error response.");
				break;
			}

			retry = aws_dynamo_parse_error_response(response, response_len, &message, &dynamodb_response_code);
			if (retry == 0) {
				/* Don't retry. */
				break;
			} else if (retry != 1) {
				Warnx("aws_dynamo_request: Error evaluating error body. target='%s' body='%s' response='%s'",
						target, body, response);
				break;
			}

			backoff = (1 << attempt) * (rand() % 50000 + 25000);
			Warnx("aws_dynamo_request: '%s' will retry after %d ms wait, attempt %d: %s %s",
				message ? message : "unknown error", backoff / 1000, attempt, target, body);
			usleep(backoff);
			attempt++;
		}

	} while (attempt < aws->dynamo_max_retries);

	if (attempt >= aws->dynamo_max_retries) {
			Warnx("aws_dynamo_request: max retry limit hit, giving up: %s %s", target, body);
	}

	if (message != NULL) {
		snprintf(aws->dynamo_message, sizeof(aws->dynamo_message), "%s",
			message);
		free(message);
		aws->dynamo_errno = dynamodb_response_code;
	} else {
		aws->dynamo_message[0] = '\0';
		aws->dynamo_errno = AWS_DYNAMO_CODE_NONE;
	}

	return rv;
}

int aws_dynamo_request(struct aws_handle *aws, const char *target, const char *body) {
	int http_response_code;
	int dynamodb_response_code = AWS_DYNAMO_CODE_UNKNOWN;
	int rv = -1;
	int attempt = 0;
	char *message = NULL;

	do {

		if (aws_dynamo_post(aws, target, body) == -1) {
			return -1;
		}
	
		http_response_code = http_get_response_code(aws->http);

		if (http_response_code == 200) {
			rv = 0;
			break;
		} else if (http_response_code ==  413) {
			Warnx("aws_dynamo_request: Request Entity Too Large. Maximum item size of 1MB exceeded.");
			break;
		} else if (http_response_code == 400 || http_response_code == 500) {
			const char *response;
			int response_len;
			int retry;
			useconds_t backoff;

			response = http_get_data(aws->http, &response_len);

			if (response == NULL) {
				Warnx("aws_dynamo_request: Failed to get error response.");
				break;
			}

			retry = aws_dynamo_parse_error_response(response, response_len, &message, &dynamodb_response_code);
			if (retry == 0) {
				/* Don't retry. */
				break;
			} else if (retry != 1) {
				Warnx("aws_dynamo_request: Error evaluating error body. target='%s' body='%s' response='%s'",
						target, body, response);
				break;
			}

			backoff = (1 << attempt) * (rand() % 50000 + 25000);
			Warnx("aws_dynamo_request: '%s' will retry after %d ms wait, attempt %d: %s %s",
				message ? message : "unknown error", backoff / 1000, attempt, target, body);
			usleep(backoff);
			attempt++;
		}

	} while (attempt < aws->dynamo_max_retries);

	if (attempt >= aws->dynamo_max_retries) {
			Warnx("aws_dynamo_request: max retry limit hit, giving up: %s %s", target, body);
	}

	if (message != NULL) {
		snprintf(aws->dynamo_message, sizeof(aws->dynamo_message), "%s",
			message);
		free(message);
		aws->dynamo_errno = dynamodb_response_code;
	} else {
		aws->dynamo_message[0] = '\0';
		aws->dynamo_errno = AWS_DYNAMO_CODE_NONE;
	}

	return rv;
}

char *aws_dynamo_get_message(struct aws_handle *aws) {
	return aws->dynamo_message;
}

int aws_dynamo_get_errno(struct aws_handle *aws) {
	return aws->dynamo_errno;
}

int aws_dynamo_json_get_double(const char *val, size_t len, double *d)
{
	char buf[32];

	if (len + 1 > sizeof(buf)) {
		Warnx("aws_dynamo_json_get_double: double string too long.");
		return -1;
	} else {
		char *endptr;
		double temp;

		snprintf(buf, len + 1, "%s", val);

		errno = 0;
		temp = strtod(buf, &endptr);

		if (errno != 0) {
			Warnx("aws_dynamo_json_get_double: double conversion failed.");
			return -1;
		}

		if (*endptr == '\0') {
			*d = temp;
		} else {
			Warnx("aws_dynamo_json_get_double: double conversion incomplete.");
			return -1;
		}
	}
	return 0;
}

int aws_dynamo_json_get_long_long_int(const unsigned char *val, size_t len, long long int *i)
{
	char buf[64];

	if (len + 1 > sizeof(buf)) {
		Warnx("aws_dynamo_json_get_long_long_int: int string too long.");
		return -1;
	} else {
		char *endptr;
		long long int temp;

		snprintf(buf, len + 1, "%s", val);

		errno = 0;
		temp = strtoll(buf, &endptr, 0);

		if (errno != 0) {
			Warnx("aws_dynamo_json_get_long_long_int: int conversion failed.");
			return -1;
		}

		if (*endptr == '\0') {
			*i = temp;
		} else {
			Warnx("aws_dynamo_json_get_long_long_int: int conversion incomplete.");
			return -1;
		}
	}
	return 0;
}

int aws_dynamo_json_get_int(const unsigned char *val, size_t len, int *i)
{
	char buf[32];

	if (len + 1 > sizeof(buf)) {
		Warnx("aws_dynamo_json_get_int: int string too long.");
		return -1;
	} else {
		char *endptr;
		int temp;

		snprintf(buf, len + 1, "%s", val);

		errno = 0;
		temp = strtol(buf, &endptr, 0);

		if (errno != 0) {
			Warnx("aws_dynamo_json_get_int: int conversion failed.");
			return -1;
		}

		if (*endptr == '\0') {
			*i = temp;
		} else {
			Warnx("aws_dynamo_json_get_int: int conversion incomplete.");
			return -1;
		}
	}
	return 0;
}

int aws_dynamo_json_get_table_status(const unsigned char *val, size_t len, enum aws_dynamo_table_status *status) {
	if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_JSON_TABLE_STATUS_CREATING, val, len)) {
			  *status = AWS_DYNAMO_TABLE_STATUS_CREATING;
	} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_JSON_TABLE_STATUS_ACTIVE, val, len)) {
			  *status = AWS_DYNAMO_TABLE_STATUS_ACTIVE;
	} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_JSON_TABLE_STATUS_DELETING, val, len)) {
			  *status = AWS_DYNAMO_TABLE_STATUS_DELETING;
	} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_JSON_TABLE_STATUS_UPDATING, val, len)) {
			  *status = AWS_DYNAMO_TABLE_STATUS_UPDATING;
	} else {
		return -1;
	}
	return 0;
}

int aws_dynamo_json_get_type(const unsigned char *val, size_t len, enum aws_dynamo_attribute_type *type) {
	if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_JSON_TYPE_STRING, val, len)) {
			  *type = AWS_DYNAMO_STRING;
	} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_JSON_TYPE_STRING_SET, val, len)) {
			  *type = AWS_DYNAMO_STRING_SET;
	} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_JSON_TYPE_NUMBER, val, len)) {
			  *type = AWS_DYNAMO_NUMBER;
	} else if (AWS_DYNAMO_VALCMP(AWS_DYNAMO_JSON_TYPE_NUMBER_SET, val, len)) {
			  *type = AWS_DYNAMO_NUMBER_SET;
	} else {
		return -1;
	}
	return 0;
}

void aws_dynamo_dump_attributes(struct aws_dynamo_attribute *attributes,
	int num_attributes) {
#ifdef DEBUG_AWS_DYNAMO
	int j;

	for (j = 0; j < num_attributes; j++) {
		struct aws_dynamo_attribute *attribute;
		
		attribute = &(attributes[j]);
		switch (attribute->type) {
			case AWS_DYNAMO_STRING: {
				if (attribute->value.string != NULL) {
					Debug("Attribute %d, String, %s=%s", j, attribute->name, attribute->value.string);
				} else {
					Debug("Attribute %d, %s is NULL", j, attribute->name);
				}
				break;
			}
			case AWS_DYNAMO_NUMBER: {
				switch (attribute->value.number.type) {
					case AWS_DYNAMO_NUMBER_INTEGER: {
						if (attribute->value.number.value.integer_val != NULL) {
							Debug("Attribute %d, Number (integer), %s=%lld", j, attribute->name,
								*(attribute->value.number.value.integer_val));
						} else {
							Debug("Attribute %d, %s is NULL", j, attribute->name);
						}
						break;
					}
					case AWS_DYNAMO_NUMBER_DOUBLE: {
						if (attribute->value.number.value.double_val != NULL) {
							Debug("Attribute %d, Number (double) %s=%Lf", j, attribute->name,
								*(attribute->value.number.value.double_val));
						} else {
							Debug("Attribute %d, %s is NULL", j, attribute->name);
						}
						break;
					}
					default: {
						Warnx("aws_dynamo_dump_attributes - Unknown number type %d",
							attribute->value.number.type);
					}
				}

				break;
			}
			case AWS_DYNAMO_STRING_SET: {

				if (attribute->value.string_set.strings != NULL) {
					int i;
					Debug("Attribute %d, String set, %s", j, attribute->name);
					for (i = 0; i < attribute->value.string_set.num_strings; i++) {
						Debug("%d - %s", i, attribute->value.string_set.strings[i]);
					}
				} else {
					Debug("Attribute %d, %s is NULL", j, attribute->name);
				}
				break;
			}
			case AWS_DYNAMO_NUMBER_SET: {
				/* Unimplemented. */
				Warnx("aws_dynamo_dump_attributes - Number sets not implemented.");
				break;
			}
			default: {
				Warnx("aws_dynamo_dump_attributes - Unknown type %d",
					attribute->type);
				break;
			}
		}
	}
#endif 
}

void aws_dynamo_free_attributes(struct aws_dynamo_attribute *attributes,
	int num_attributes) {
	int j;

	for (j = 0; j < num_attributes; j++) {
		struct aws_dynamo_attribute *attribute;

		attribute = &(attributes[j]);
		switch (attribute->type) {
			case AWS_DYNAMO_STRING: {
				free(attribute->value.string);
				break;
			}
			case AWS_DYNAMO_NUMBER: {
				switch (attribute->value.number.type) {
					case AWS_DYNAMO_NUMBER_INTEGER: {
						free(attribute->value.number.value.integer_val);
						break;
					}
					case AWS_DYNAMO_NUMBER_DOUBLE: {
						free(attribute->value.number.value.double_val);
						break;
					}
					default: {
						Warnx("aws_dynamo_free_attributes - Unknown number type %d",
							attribute->value.number.type);
					}
				}

				break;
			}
			case AWS_DYNAMO_STRING_SET: {
				int i;

				for (i = 0; i < attribute->value.string_set.num_strings; i++) {
					free(attribute->value.string_set.strings[i]);
				}
				free(attribute->value.string_set.strings);

				break;
			}
			case AWS_DYNAMO_NUMBER_SET: {
				/* Unimplemented. */
				Warnx("aws_dynamo_free_attributes - Number sets not implemented.");
				break;
			}
			default: {
				Warnx("aws_dynamo_free_attributes - Unknown type %d",
					attribute->type);
				break;
			}
		}
	}
	free(attributes);
}

void aws_dynamo_free_item(struct aws_dynamo_item *item) {
	if (item == NULL) {
		return;
	}
	aws_dynamo_free_attributes(item->attributes, item->num_attributes);
	free(item);
}

int aws_dynamo_parse_attribute_value(struct aws_dynamo_attribute *attribute, const unsigned char *val,  size_t len)
{
	switch (attribute->type) {
		case AWS_DYNAMO_NUMBER: {
			switch (attribute->value.number.type) {
				case AWS_DYNAMO_NUMBER_INTEGER: {
					aws_dynamo_integer_t lli;

					if (aws_dynamo_json_get_long_long_int(val, len, &lli) == -1) {
						Warnx("aws_dynamo_parse_attribute_value: failed to parse number");
						return 0;
					}

					if (attribute->value.number.value.integer_val != NULL) {
						Warnx("aws_dynamo_parse_attribute_value: number is already set?");
						return 0;
					}

					attribute->value.number.value.integer_val = calloc(sizeof(aws_dynamo_integer_t), 1);
					if (attribute->value.number.value.integer_val == NULL) {
						Warnx("aws_dynamo_parse_attribute_value: number alloc failed");
						return 0;
					}
	
					*(attribute->value.number.value.integer_val) = lli;
					break;
				}
				case AWS_DYNAMO_NUMBER_DOUBLE: {
					/* Double conversion is not implemented for now since we don't need it. */
					Warnx("aws_dynamo_parse_attribute_value: double conversion not implemented");
					return 0;

					break;
				}
				default: {
					Warnx("aws_dynamo_parse_attribute_value: unknown number type.");
					return 0;
				}

			}
			break;
		}
		case AWS_DYNAMO_STRING: {
			attribute->value.string = strndup(val, len);
			break;
		}
		case AWS_DYNAMO_STRING_SET: {
			char **strings;

			strings = realloc(attribute->value.string_set.strings,
				sizeof(*strings) * (attribute->value.string_set.num_strings + 1));

			if (strings == NULL) {
				Warnx("aws_dynamo_parse_attribute_value: string set realloc failed.");
				return 0;
			}

			attribute->value.string_set.strings = strings;
			attribute->value.string_set.strings[attribute->value.string_set.num_strings] = strndup(val, len);
			attribute->value.string_set.num_strings++;

			break;
		}
		case AWS_DYNAMO_NUMBER_SET: {
			Warnx("aws_dynamo_parse_attribute_value: number set not supported");
			return 0;
			break;
		}
		default: {
			Warnx("aws_dynamo_parse_attribute_value: unsupported attribute type - %d", attribute->type);
			return 0;
			break;
		}
	}
	return 1;
}

struct aws_dynamo_item *aws_dynamo_copy_item(struct aws_dynamo_item *item) {
	struct aws_dynamo_item *copy;
	int j;

	copy = calloc(1, sizeof(*copy));

	if (copy == NULL) {
		Warnx("aws_dynamo_copy_item: item calloc() failed.");
		return NULL;
	}

	copy->attributes = calloc(item->num_attributes, sizeof(*(copy->attributes)));

	if (copy->attributes == NULL) {
		Warnx("aws_dynamo_copy_item: attribute calloc() failed.");
		free(copy);
		return NULL;
	}

	for (j = 0; j < item->num_attributes; j++) {
		struct aws_dynamo_attribute *attribute;

		attribute = &(item->attributes[j]);
		copy->attributes[j].name = attribute->name; /* These strings are const. */
		copy->attributes[j].name_len = attribute->name_len;
		copy->attributes[j].type = attribute->type;
		switch (attribute->type) {
			case AWS_DYNAMO_STRING: {
				if (attribute->value.string != NULL) {
					copy->attributes[j].value.string = strdup(attribute->value.string);
					if (copy->attributes[j].value.string == NULL) {
						Warnx("aws_dynamo_copy_item: strdup() failed.");
						goto error;
					}
				}
				break;
			}
			case AWS_DYNAMO_NUMBER: {
				switch (attribute->value.number.type) {
					case AWS_DYNAMO_NUMBER_INTEGER: {
						copy->attributes[j].value.number.type = AWS_DYNAMO_NUMBER_INTEGER;
						if (attribute->value.number.value.integer_val != NULL) {
							copy->attributes[j].value.number.value.integer_val = calloc(sizeof(aws_dynamo_integer_t), 1);
							if (copy->attributes[j].value.number.value.integer_val == NULL) {
								Warnx("aws_dynamo_copy_item: calloc() for int val failed.");
								goto error;
							} 
							*(copy->attributes[j].value.number.value.integer_val) = *(attribute->value.number.value.integer_val);
						}
						break;
					}
					case AWS_DYNAMO_NUMBER_DOUBLE: {
						Warnx("aws_dynamo_copy_item: double support not implemented");
						break;
					}
					default: {
						Warnx("aws_dynamo_copy_item - Unknown number type %d",
							attribute->value.number.type);
					}
				}

				break;
			}
			case AWS_DYNAMO_STRING_SET: {
				char **strings;
				int i;

				if (attribute->value.string_set.strings != NULL) {
					strings = calloc(attribute->value.string_set.num_strings, sizeof(*strings));
					if (strings == NULL) {
						Warnx("aws_dynamo_copy_item: calloc() for strings failed.");
						goto error;
					}

					for (i = 0; i < attribute->value.string_set.num_strings; i++) {
						strings[i] = strdup(attribute->value.string_set.strings[i]);
						if (strings[i] == NULL) {
							Warnx("aws_dynamo_copy_item: calloc() for string failed.");
							free(strings);
							goto error;
						}
					}
				}

				break;
			}
			case AWS_DYNAMO_NUMBER_SET: {
				/* Unimplemented. */
				Warnx("aws_dynamo_copy_item - Number sets not implemented.");
				break;
			}
			default: {
				Warnx("aws_dynamo_copy_item - Unknown type %d",
					attribute->type);
				break;
			}
		}
		copy->num_attributes++;
	}

	return copy;

error:
	aws_dynamo_free_attributes(copy->attributes, copy->num_attributes);
	free(copy);
	return NULL;
}

void aws_dynamo_set_max_retries(struct aws_handle *aws, int dynamo_max_retries) {
	aws->dynamo_max_retries = dynamo_max_retries;
}

void aws_dynamo_set_https(struct aws_handle *aws, int https) {
	aws->dynamo_https = https;
}

void aws_dynamo_set_https_certificate_file(struct aws_handle *aws, const char *filename) {
	http_set_https_certificate_file(aws->http, filename);
}

int aws_dynamo_set_endpoint(struct aws_handle *aws, const char *host) {
	char *h;

	h = strdup(host);
	if (h == NULL) {
		Warnx("aws_dynamo_set_endpoint: failed ot allocate host");
		return -1;
	}	
	aws->dynamo_host = h;
	return 0;
}

void aws_dynamo_set_port(struct aws_handle *aws, int port) {
	aws->dynamo_port = port;
}
