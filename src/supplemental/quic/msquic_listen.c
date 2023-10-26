//
// Copyright 2023 NanoMQ Team, Inc. <wangwei@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// #if defined(NNG_ENABLE_QUIC) // && defined(NNG_QUIC_MSQUIC)
//
// Note.
// Quic connection is only visible in nng stream.
// Each nng stream is linked to a quic stream.
// nng dialer is linked to quic connection.
// The quic connection would be established when the first
// nng stream with same URL is created.
// The quic connection would be free if all nng streams
// closed.

#include "quic_api.h"
#include "core/nng_impl.h"
#include "msquic.h"

#include "nng/mqtt/mqtt_client.h"
#include "nng/supplemental/nanolib/conf.h"
#include "nng/protocol/mqtt/mqtt_parser.h"
#include "supplemental/mqtt/mqtt_msg.h"

#include "openssl/pem.h"
#include "openssl/x509.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nng/mqtt/mqtt_client.h"
#include "nng/supplemental/nanolib/conf.h"
#include "nng/protocol/mqtt/mqtt_parser.h"
#include "supplemental/mqtt/mqtt_msg.h"

#include "openssl/pem.h"
#include "openssl/x509.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct nni_quic_conn {
	nng_stream      stream;
	nni_list        readq;
	nni_list        writeq;
	bool            closed;
	nni_mtx         mtx;
	nni_aio *       dial_aio;
	// nni_aio *       qstrmaio; // Link to msquic_strm_cb
	nni_quic_dialer *dialer;

	// MsQuic
	HQUIC           qstrm; // quic stream
	uint8_t         reason_code;

	nni_reap_node   reap;
};

static const QUIC_API_TABLE *MsQuic = NULL;

// Config for msquic
static const QUIC_REGISTRATION_CONFIG quic_reg_config = {
	"mqtt_listener",
	QUIC_EXECUTION_PROFILE_LOW_LATENCY
};

static const QUIC_BUFFER quic_alpn = {
	sizeof("mqtt") - 1,
	(uint8_t *) "mqtt"
};

HQUIC registration;
HQUIC configuration

/***************************** MsQuic Listener ******************************/

int
nni_quic_listener_init(void **argp)
{
	nni_quic_listener *l;

	if ((l = NNI_ALLOC_STRUCT(l)) == NULL) {
		return (NNG_ENOMEM);
	}

	nni_mtx_init(&l->mtx);

	l->closed  = false;
	l->started = false;

	l->ql      = NULL;

	// nni_aio_alloc(&l->qconaio, quic_listener_cb, (void *)l);
	nni_aio_list_init(&l->acceptq);
	nni_atomic_init_bool(&l->fini);
	nni_atomic_init64(&l->ref);
	nni_atomic_inc64(&l->ref);

	// 0RTT is disabled by default
	l->enable_0rtt = false;
	// multi_stream is disabled by default
	l->enable_mltstrm = false;

	memset(&l->settings, 0, sizeof(QUIC_SETTINGS));

	*argp = l;
	return 0;
}

int
nni_quic_listener_listen(nni_quic_listener *l, const char *h, const char *p)
{
	socklen_t               len;
	int                     rv;
	int                     fd;
	nni_posix_pfd *         pfd;

	nni_mtx_lock(&l->mtx);
	if (l->started) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ESTATE);
	}
	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ECLOSED);
	}

	msquic_listen(l->ql, h, p);

	l->started = true;
	nni_mtx_unlock(&l->mtx);

	return (0);
}

/***************************** MsQuic Bindings *****************************/

static void
msquic_load_listener_config()
{
	return;
}

static int
msquic_listen(HQUIC ql, const char *h, const char *p)
{
	HQUIC addr;
	QUIC_STATUS rv = 0;

	QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
	QuicAddrSetPort(&addr, atoi(p));

	msquic_load_listener_config();

	if (QUIC_FAILED(rv = MsQuic->ListenerOpen(registration, msquic_listener_cb, NULL, &ql))) {
		log_error("error in listen open %ld", rv);
		goto error;
	}

	if (QUIC_FAILED(rv = MsQuic->ListenerStart(ql, alpn, 1, &addr))) {
		log_error("error in listen start %ld", rv);
		goto error;
	}

	return rv;

error:
	if (ql != NULL) {
		MsQuic->ListenerClose(ql);
	}
	return rv;
}