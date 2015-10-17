/*
 * listen.c	Handle socket stuff
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2005,2006  The FreeRADIUS server project
 * Copyright 2005  Alan DeKok <aland@ox.org>
 */

RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/rad_assert.h>
#include <freeradius-devel/process.h>
#include <freeradius-devel/protocol.h>
#include <freeradius-devel/modpriv.h>

#include <freeradius-devel/detail.h>

#ifdef WITH_UDPFROMTO
#  include <freeradius-devel/udpfromto.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
#endif

#ifdef HAVE_NET_IF_H
#  include <net/if.h>
#endif

#ifdef HAVE_FCNTL_H
  #include <fcntl.h>
#endif

#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif

#ifdef DEBUG_PRINT_PACKET
static void print_packet(RADIUS_PACKET *packet)
{
	char src[256], dst[256];

	fr_inet_ntoh(&packet->src_ipaddr, src, sizeof(src));
	fr_inet_ntoh(&packet->dst_ipaddr, dst, sizeof(dst));

	fprintf(stderr, "ID %d: %s %d -> %s %d\n", packet->id,
		src, packet->src_port, dst, packet->dst_port);

}
#endif


static rad_listen_t *listen_alloc(TALLOC_CTX *ctx, RAD_LISTEN_TYPE type);

#ifdef WITH_COMMAND_SOCKET
static int command_tcp_recv(rad_listen_t *listener);
static int command_tcp_send(rad_listen_t *listener, REQUEST *request);
static int command_write_magic(int newfd, listen_socket_t *sock);
#endif

static int last_listener = RAD_LISTEN_MAX;
#define MAX_LISTENER (256)
static fr_protocol_t master_listen[MAX_LISTENER];

/*
 *	Find a per-socket client.
 */
RADCLIENT *client_listener_find(rad_listen_t *listener,
				fr_ipaddr_t const *ipaddr, uint16_t src_port)
{
#ifdef WITH_DYNAMIC_CLIENTS
	int rcode;
	REQUEST *request;
	RADCLIENT *created;
#endif
	time_t now;
	RADCLIENT *client;
	RADCLIENT_LIST *clients;
	listen_socket_t *sock;

	rad_assert(listener != NULL);
	rad_assert(ipaddr != NULL);

	sock = listener->data;
	clients = sock->clients;

	/*
	 *	This HAS to have been initialized previously.
	 */
	rad_assert(clients != NULL);

	client = client_find(clients, ipaddr, sock->proto);
	if (!client) {
		char name[256], buffer[INET6_ADDRSTRLEN];

#ifdef WITH_DYNAMIC_CLIENTS
	unknown:		/* used only for dynamic clients */
#endif

		/*
		 *	DoS attack quenching, but only in daemon mode.
		 *	If they're running in debug mode, show them
		 *	every packet.
		 */
		if (rad_debug_lvl == 0) {
			static time_t last_printed = 0;

			now = time(NULL);
			if (last_printed == now) return NULL;

			last_printed = now;
		}

		listener->print(listener, name, sizeof(name));

		radlog(L_ERR, "Ignoring request to %s from unknown client %s port %d"
#ifdef WITH_TCP
		       " proto %s"
#endif
		       , name, inet_ntop(ipaddr->af, &ipaddr->ipaddr,
					 buffer, sizeof(buffer)), src_port
#ifdef WITH_TCP
		       , (sock->proto == IPPROTO_UDP) ? "udp" : "tcp"
#endif
		       );
		return NULL;
	}

#ifndef WITH_DYNAMIC_CLIENTS
	return client;		/* return the found client. */
#else

	/*
	 *	No server defined, and it's not dynamic.  Return it.
	 */
	if (!client->client_server && !client->dynamic) return client;

	now = time(NULL);

	/*
	 *	It's a dynamically generated client, check it.
	 */
	if (client->dynamic && (src_port != 0)) {
#  ifdef HAVE_SYS_STAT_H
		char const *filename;
#  endif

		/*
		 *	Lives forever.  Return it.
		 */
		if (client->lifetime == 0) return client;

		/*
		 *	Rate-limit the deletion of known clients.
		 *	This makes them last a little longer, but
		 *	prevents the server from melting down if (say)
		 *	10k clients all expire at once.
		 */
		if (now == client->last_new_client) return client;

		/*
		 *	It's not dead yet.  Return it.
		 */
		if ((client->created + client->lifetime) > now) return client;

#  ifdef HAVE_SYS_STAT_H
		/*
		 *	The client was read from a file, and the file
		 *	hasn't changed since the client was created.
		 *	Just renew the creation time, and continue.
		 *	We don't need to re-load the same information.
		 */
		if (client->cs &&
		    (filename = cf_section_filename(client->cs)) != NULL) {
			struct stat buf;

			if ((stat(filename, &buf) >= 0) &&
			    (buf.st_mtime < client->created)) {
				client->created = now;
				return client;
			}
		}
#  endif


		/*
		 *	This really puts them onto a queue for later
		 *	deletion.
		 */
		client_delete(clients, client);

		/*
		 *	Go find the enclosing network again.
		 */
		client = client_find(clients, ipaddr, sock->proto);

		/*
		 *	WTF?
		 */
		if (!client) goto unknown;
		if (!client->client_server) goto unknown;

		/*
		 *	At this point, 'client' is the enclosing
		 *	network that configures where dynamic clients
		 *	can be defined.
		 */
		rad_assert(client->dynamic == 0);

	} else if (!client->dynamic && client->rate_limit) {
		/*
		 *	The IP is unknown, so we've found an enclosing
		 *	network.  Enable DoS protection.  We only
		 *	allow one new client per second.  Known
		 *	clients aren't subject to this restriction.
		 */
		if (now == client->last_new_client) goto unknown;
	}

	client->last_new_client = now;

	request = request_alloc(NULL);
	if (!request) goto unknown;

	request->listener = listener;
	request->client = client;
	request->packet = rad_recv(NULL, listener->fd, 0x02); /* MSG_PEEK */
	if (!request->packet) {				/* badly formed, etc */
		talloc_free(request);
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		goto unknown;
	}
	(void) talloc_steal(request, request->packet);
	request->reply = rad_alloc_reply(request, request->packet);
	if (!request->reply) {
		talloc_free(request);
		goto unknown;
	}
	gettimeofday(&request->packet->timestamp, NULL);
	request->number = 0;
	request->priority = listener->type;
	request->server = client->client_server;
	request->root = &main_config;

	/*
	 *	Run a fake request through the given virtual server.
	 *	Look for FreeRADIUS-Client-IP-Address
	 *		 FreeRADIUS-Client-Secret
	 *		...
	 *
	 *	and create the RADCLIENT structure from that.
	 */
	RDEBUG("server %s {", request->server);

	rcode = process_authorize(0, request);

	RDEBUG("} # server %s", request->server);

	switch (rcode) {
	case RLM_MODULE_OK:
	case RLM_MODULE_UPDATED:
		break;

	/*
	 *	Likely a fatal error we want to warn the user about
	 */
	case RLM_MODULE_INVALID:
	case RLM_MODULE_FAIL:
		ERROR("Virtual-Server %s returned %s, creating dynamic client failed", request->server,
		      fr_int2str(mod_rcode_table, rcode, "<INVALID>"));
		talloc_free(request);
		goto unknown;

	/*
	 *	Probably the result of policy, or the client not existing.
	 */
	default:
		DEBUG("Virtual-Server %s returned %s, ignoring client", request->server,
		      fr_int2str(mod_rcode_table, rcode, "<INVALID>"));
		talloc_free(request);
		goto unknown;
	}

	/*
	 *	If the client was updated by rlm_dynamic_clients,
	 *	don't create the client from attribute-value pairs.
	 */
	if (request->client == client) {
		created = client_afrom_request(clients, request);
	} else {
		created = request->client;

		/*
		 *	This frees the client if it isn't valid.
		 */
		if (!client_add_dynamic(clients, client, created)) goto unknown;
	}

	request->server = client->server;
	exec_trigger(request, NULL, "server.client.add", false);

	talloc_free(request);

	if (!created) goto unknown;

	return created;
#endif
}

static int listen_bind(rad_listen_t *this);

#ifdef HAVE_LIBPCAP
static int init_pcap(rad_listen_t *this);
#endif

/*
 *	Process and reply to a server-status request.
 *	Like rad_authenticate and rad_accounting this should
 *	live in it's own file but it's so small we don't bother.
 */
int rad_status_server(REQUEST *request)
{
	int rcode = RLM_MODULE_OK;
	DICT_VALUE *dval;

	switch (request->listener->type) {
#ifdef WITH_STATS
	case RAD_LISTEN_NONE:
#endif
	case RAD_LISTEN_AUTH:
		dval = dict_valbyname(PW_AUTZ_TYPE, 0, "Status-Server");
		if (dval) {
			rcode = process_authorize(dval->value, request);
		} else {
			rcode = RLM_MODULE_OK;
		}

		switch (rcode) {
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
			request->reply->code = PW_CODE_ACCESS_ACCEPT;
			break;

		case RLM_MODULE_FAIL:
		case RLM_MODULE_HANDLED:
			request->reply->code = 0; /* don't reply */
			break;

		default:
		case RLM_MODULE_REJECT:
			request->reply->code = PW_CODE_ACCESS_REJECT;
			break;
		}
		break;

#ifdef WITH_ACCOUNTING
	case RAD_LISTEN_ACCT:
		dval = dict_valbyname(PW_ACCT_TYPE, 0, "Status-Server");
		if (dval) {
			rcode = process_accounting(dval->value, request);
		} else {
			rcode = RLM_MODULE_OK;
		}

		switch (rcode) {
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
			request->reply->code = PW_CODE_ACCOUNTING_RESPONSE;
			break;

		default:
			request->reply->code = 0; /* don't reply */
			break;
		}
		break;
#endif

#ifdef WITH_COA
		/*
		 *	This is a vendor extension.  Suggested by Glen
		 *	Zorn in IETF 72, and rejected by the rest of
		 *	the WG.  We like it, so it goes in here.
		 */
	case RAD_LISTEN_COA:
		dval = dict_valbyname(PW_RECV_COA_TYPE, 0, "Status-Server");
		if (dval) {
			rcode = process_recv_coa(dval->value, request);
		} else {
			rcode = RLM_MODULE_OK;
		}

		switch (rcode) {
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
			request->reply->code = PW_CODE_COA_ACK;
			break;

		default:
			request->reply->code = 0; /* don't reply */
			break;
		}
		break;
#endif

	default:
		return 0;
	}

#ifdef WITH_STATS
	/*
	 *	Full statistics are available only on a statistics
	 *	socket.
	 */
	if (request->listener->type == RAD_LISTEN_NONE) {
		request_stats_reply(request);
	}
#endif

	return 0;
}

#ifdef WITH_TCP
static int dual_tcp_recv(rad_listen_t *listener)
{
	int rcode;
	RADIUS_PACKET	*packet;
	RAD_REQUEST_FUNP fun = NULL;
	listen_socket_t *sock = listener->data;
	RADCLIENT	*client = sock->client;

	rad_assert(client != NULL);

	if (listener->status != RAD_LISTEN_STATUS_KNOWN) return 0;

	/*
	 *	Allocate a packet for partial reads.
	 */
	if (!sock->packet) {
		sock->packet = rad_alloc(sock, false);
		if (!sock->packet) return 0;

		sock->packet->sockfd = listener->fd;
		sock->packet->src_ipaddr = sock->other_ipaddr;
		sock->packet->src_port = sock->other_port;
		sock->packet->dst_ipaddr = sock->my_ipaddr;
		sock->packet->dst_port = sock->my_port;
		sock->packet->proto = sock->proto;
	}

	/*
	 *	Grab the packet currently being processed.
	 */
	packet = sock->packet;

	rcode = fr_tcp_read_packet(packet, 0);

	/*
	 *	Still only a partial packet.  Put it back, and return,
	 *	so that we'll read more data when it's ready.
	 */
	if (rcode == 0) {
		return 0;
	}

	if (rcode == -1) {	/* error reading packet */
		char buffer[256];

		ERROR("Invalid packet from %s port %d, closing socket: %s",
		       fr_inet_ntoh(&packet->src_ipaddr, buffer, sizeof(buffer)),
		       packet->src_port, fr_strerror());
	}

	if (rcode < 0) {	/* error or connection reset */
		listener->status = RAD_LISTEN_STATUS_EOL;

		/*
		 *	Tell the event handler that an FD has disappeared.
		 */
		DEBUG("Client has closed connection");
		radius_update_listener(listener);

		/*
		 *	Do NOT free the listener here.  It's in use by
		 *	a request, and will need to hang around until
		 *	all of the requests are done.
		 *
		 *	It is instead free'd in remove_from_request_hash()
		 */
		return 0;
	}

	/*
	 *	Some sanity checks, based on the packet code.
	 */
	switch (packet->code) {
	case PW_CODE_ACCESS_REQUEST:
		if (listener->type != RAD_LISTEN_AUTH) goto bad_packet;
		FR_STATS_INC(auth, total_requests);
		fun = rad_authenticate;
		break;

#  ifdef WITH_ACCOUNTING
	case PW_CODE_ACCOUNTING_REQUEST:
		if (listener->type != RAD_LISTEN_ACCT) goto bad_packet;
		FR_STATS_INC(acct, total_requests);
		fun = rad_accounting;
		break;
#  endif

	case PW_CODE_STATUS_SERVER:
		if (!main_config.status_server) {
			FR_STATS_INC(auth, total_unknown_types);
			WARN("Ignoring Status-Server request due to security configuration");
			rad_free(&sock->packet);
			return 0;
		}
		fun = rad_status_server;
		break;

	default:
	bad_packet:
		FR_STATS_INC(auth, total_unknown_types);

		DEBUG("Invalid packet code %d sent from client %s port %d : IGNORED",
		      packet->code, client->shortname, packet->src_port);
		rad_free(&sock->packet);
		return 0;
	} /* switch over packet types */

	if (!request_receive(NULL, listener, packet, client, fun)) {
		FR_STATS_INC(auth, total_packets_dropped);
		rad_free(&sock->packet);
		return 0;
	}

	sock->packet = NULL;	/* we have no need for more partial reads */
	return 1;
}


static int dual_tcp_accept(rad_listen_t *listener)
{
	int newfd;
	uint16_t src_port;
	rad_listen_t *this;
	socklen_t salen;
	struct sockaddr_storage src;
	listen_socket_t *sock;
	fr_ipaddr_t src_ipaddr;
	RADCLIENT *client = NULL;

	salen = sizeof(src);

	DEBUG2(" ... new connection request on TCP socket");

	newfd = accept(listener->fd, (struct sockaddr *) &src, &salen);
	if (newfd < 0) {
		/*
		 *	Non-blocking sockets must handle this.
		 */
#  ifdef EWOULDBLOCK
		if (errno == EWOULDBLOCK) {
			return 0;
		}
#  endif

		DEBUG2(" ... failed to accept connection");
		return -1;
	}

	if (!fr_ipaddr_from_sockaddr(&src, salen, &src_ipaddr, &src_port)) {
		close(newfd);
		DEBUG2(" ... unknown address family");
		return 0;
	}

	/*
	 *	Enforce client IP address checks on accept, not on
	 *	every packet.
	 */
	if ((client = client_listener_find(listener,
					   &src_ipaddr, src_port)) == NULL) {
		close(newfd);
		FR_STATS_INC(auth, total_invalid_requests);
		return 0;
	}

#  ifdef WITH_TLS
	/*
	 *	Enforce security restrictions.
	 *
	 *	This shouldn't be necessary in practice.  However, it
	 *	serves as a double-check on configurations.  Marking a
	 *	client as "tls required" means that any accidental
	 *	exposure of the client to non-TLS traffic is
	 *	prevented.
	 */
	if (client->tls_required && !listener->tls) {
		INFO("Ignoring connection to TLS socket from non-TLS client");
		close(newfd);
		return 0;
	}
#  endif

	/*
	 *	Enforce max_connections on client && listen section.
	 */
	if ((client->limit.max_connections != 0) &&
	    (client->limit.max_connections == client->limit.num_connections)) {
		/*
		 *	FIXME: Print client IP/port, and server IP/port.
		 */
		INFO("Ignoring new connection due to client max_connections (%d)", client->limit.max_connections);
		close(newfd);
		return 0;
	}

	sock = listener->data;
	if ((sock->limit.max_connections != 0) &&
	    (sock->limit.max_connections == sock->limit.num_connections)) {
		/*
		 *	FIXME: Print client IP/port, and server IP/port.
		 */
		INFO("Ignoring new connection due to socket max_connections");
		close(newfd);
		return 0;
	}
	client->limit.num_connections++;
	sock->limit.num_connections++;

	/*
	 *	Add the new listener.  We require a new context here,
	 *	because the allocations for the packet, etc. in the
	 *	child listener will be done in a child thread.
	 */
	this = listen_alloc(NULL, listener->type);
	if (!this) return -1;

	/*
	 *	Copy everything, including the pointer to the socket
	 *	information.
	 */
	sock = this->data;
	memcpy(this->data, listener->data, sizeof(*sock));
	memcpy(this, listener, sizeof(*this));
	this->next = NULL;
	this->data = sock;	/* fix it back */

	sock->parent = listener->data;
	sock->other_ipaddr = src_ipaddr;
	sock->other_port = src_port;
	sock->client = client;
	sock->opened = sock->last_packet = time(NULL);

	/*
	 *	Set the limits.  The defaults are the parent limits.
	 *	Client limits on max_connections are enforced dynamically.
	 *	Set the MINIMUM of client/socket idle timeout or lifetime.
	 */
	memcpy(&sock->limit, &sock->parent->limit, sizeof(sock->limit));

	if (client->limit.idle_timeout &&
	    ((sock->limit.idle_timeout == 0) ||
	     (client->limit.idle_timeout < sock->limit.idle_timeout))) {
		sock->limit.idle_timeout = client->limit.idle_timeout;
	}

	if (client->limit.lifetime &&
	    ((sock->limit.lifetime == 0) ||
	     (client->limit.lifetime < sock->limit.lifetime))) {
		sock->limit.lifetime = client->limit.lifetime;
	}

	this->fd = newfd;
	this->status = RAD_LISTEN_STATUS_INIT;

	this->parent = listener;
	if (!rbtree_insert(listener->children, this)) {
		ERROR("Failed inserting TCP socket into parent list.");
	}

#  ifdef WITH_COMMAND_SOCKET
	if (this->type == RAD_LISTEN_COMMAND) {
		this->recv = command_tcp_recv;
		this->send = command_tcp_send;
		command_write_magic(this->fd, sock);
	} else
#  endif
	{

		this->recv = dual_tcp_recv;

#  ifdef WITH_TLS
		if (this->tls) {
			this->recv = dual_tls_recv;
			this->send = dual_tls_send;
		}
#  endif
	}

	/*
	 *	FIXME: set O_NONBLOCK on the accept'd fd.
	 *	See djb's portability rants for details.
	 */

	/*
	 *	Tell the event loop that we have a new FD.
	 *	This can be called from a child thread...
	 */
	radius_update_listener(this);

	return 0;
}
#endif

/*
 *	Ensure that we always keep the correct counters.
 */
#ifdef WITH_TCP
static void common_socket_free(rad_listen_t *this)
{
	listen_socket_t *sock = this->data;

	if (sock->proto != IPPROTO_TCP) return;

	/*
	 *      Decrement the number of connections.
	 */
	if (sock->parent && (sock->parent->limit.num_connections > 0)) {
		sock->parent->limit.num_connections--;
	}
	if (sock->client && sock->client->limit.num_connections > 0) {
		sock->client->limit.num_connections--;
	}
	if (sock->home && sock->home->limit.num_connections > 0) {
		sock->home->limit.num_connections--;
	}
}
#else
#  define common_socket_free NULL
#endif

/*
 *	This function is stupid and complicated.
 */
int common_socket_print(rad_listen_t const *this, char *buffer, size_t bufsize)
{
	size_t len;
	listen_socket_t *sock = this->data;
	char const *name = master_listen[this->type].name;

#define FORWARD len = strlen(buffer); if (len >= (bufsize + 1)) return 0;buffer += len;bufsize -= len
#define ADDSTRING(_x) strlcpy(buffer, _x, bufsize);FORWARD

	ADDSTRING(name);

	if (this->dual) {
		ADDSTRING("+acct");
	}

	if (sock->interface) {
		ADDSTRING(" interface ");
		ADDSTRING(sock->interface);
	}

#ifdef WITH_TCP
	if (this->recv == dual_tcp_accept) {
		ADDSTRING(" proto tcp");
	}
#endif

#ifdef WITH_TCP
	/*
	 *	TCP sockets get printed a little differently, to make
	 *	it clear what's going on.
	 */
	if (sock->client) {
		ADDSTRING(" from client (");
		fr_inet_ntoh(&sock->other_ipaddr, buffer, bufsize);
		FORWARD;

		ADDSTRING(", ");
		snprintf(buffer, bufsize, "%d", sock->other_port);
		FORWARD;
		ADDSTRING(") -> (");

		if ((sock->my_ipaddr.af == AF_INET) &&
		    (sock->my_ipaddr.ipaddr.ip4addr.s_addr == htonl(INADDR_ANY))) {
			strlcpy(buffer, "*", bufsize);
		} else {
			fr_inet_ntoh(&sock->my_ipaddr, buffer, bufsize);
		}
		FORWARD;

		ADDSTRING(", ");
		snprintf(buffer, bufsize, "%d", sock->my_port);
		FORWARD;

		if (this->server) {
			ADDSTRING(", virtual-server=");
			ADDSTRING(this->server);
		}

		ADDSTRING(")");

		return 1;
	}

#ifdef WITH_PROXY
	/*
	 *	Maybe it's a socket that we opened to a home server.
	 */
	if ((sock->proto == IPPROTO_TCP) &&
	    (this->type == RAD_LISTEN_PROXY)) {
		ADDSTRING(" (");
		fr_inet_ntoh(&sock->my_ipaddr, buffer, bufsize);
		FORWARD;

		ADDSTRING(", ");
		snprintf(buffer, bufsize, "%d", sock->my_port);
		FORWARD;
		ADDSTRING(") -> home_server (");

		if ((sock->other_ipaddr.af == AF_INET) &&
		    (sock->other_ipaddr.ipaddr.ip4addr.s_addr == htonl(INADDR_ANY))) {
			strlcpy(buffer, "*", bufsize);
		} else {
			fr_inet_ntoh(&sock->other_ipaddr, buffer, bufsize);
		}
		FORWARD;

		ADDSTRING(", ");
		snprintf(buffer, bufsize, "%d", sock->other_port);
		FORWARD;

		ADDSTRING(")");

		return 1;
	}
#endif	/* WITH_PROXY */
#endif	/* WITH_TCP */

	ADDSTRING(" address ");

	if ((sock->my_ipaddr.af == AF_INET) &&
	    (sock->my_ipaddr.ipaddr.ip4addr.s_addr == htonl(INADDR_ANY))) {
		strlcpy(buffer, "*", bufsize);
	} else {
		fr_inet_ntoh(&sock->my_ipaddr, buffer, bufsize);
	}
	FORWARD;

	ADDSTRING(" port ");
	snprintf(buffer, bufsize, "%d", sock->my_port);
	FORWARD;

#ifdef WITH_TLS
	if (this->tls) {
		ADDSTRING(" (TLS)");
		FORWARD;
	}
#endif

	if (this->server) {
		ADDSTRING(" bound to server ");
		strlcpy(buffer, this->server, bufsize);
	}

#undef ADDSTRING
#undef FORWARD

	return 1;
}

/*
 *	Debug the packet if requested.
 */
void common_packet_debug(REQUEST *request, RADIUS_PACKET *packet, bool received)
{
	char src_ipaddr[INET6_ADDRSTRLEN];
	char dst_ipaddr[INET6_ADDRSTRLEN];
#if defined(WITH_UDPFROMTO) && defined(WITH_IFINDEX_RESOLUTION)
	char if_name[IFNAMSIZ];
#endif

	if (!packet) return;
	if (!RDEBUG_ENABLED) return;

	/*
	 *	Client-specific debugging re-prints the input
	 *	packet into the client log.
	 *
	 *	This really belongs in a utility library
	 */
	if (is_radius_code(packet->code)) {
		radlog_request(L_DBG, L_DBG_LVL_1, request, "%s %s Id %i from %s%s%s:%i to %s%s%s:%i "
#if defined(WITH_UDPFROMTO) && defined(WITH_IFINDEX_RESOLUTION)
			       "%s%s%s"
#endif
			       "length %zu",
			       received ? "Received" : "Sent",
			       fr_packet_codes[packet->code],
			       packet->id,
			       packet->src_ipaddr.af == AF_INET6 ? "[" : "",
			       inet_ntop(packet->src_ipaddr.af,
					 &packet->src_ipaddr.ipaddr,
					 src_ipaddr, sizeof(src_ipaddr)),
			       packet->src_ipaddr.af == AF_INET6 ? "]" : "",
			       packet->src_port,
			       packet->dst_ipaddr.af == AF_INET6 ? "[" : "",
			       inet_ntop(packet->dst_ipaddr.af,
					 &packet->dst_ipaddr.ipaddr,
					 dst_ipaddr, sizeof(dst_ipaddr)),
			       packet->dst_ipaddr.af == AF_INET6 ? "]" : "",
			       packet->dst_port,
#if defined(WITH_UDPFROMTO) && defined(WITH_IFINDEX_RESOLUTION)
			       packet->if_index ? "via " : "",
			       packet->if_index ? fr_ifname_from_ifindex(if_name, packet->if_index) : "",
			       packet->if_index ? " " : "",
#endif
			       packet->data_len);
	} else {
		radlog_request(L_DBG, L_DBG_LVL_1, request, "%s code %u Id %i from %s%s%s:%i to %s%s%s:%i "
#if defined(WITH_UDPFROMTO) && defined(WITH_IFINDEX_RESOLUTION)
			       "%s%s%s"
#endif
			       "length %zu",
			       received ? "Received" : "Sent",
			       packet->code,
			       packet->id,
			       packet->src_ipaddr.af == AF_INET6 ? "[" : "",
			       inet_ntop(packet->src_ipaddr.af,
					 &packet->src_ipaddr.ipaddr,
					 src_ipaddr, sizeof(src_ipaddr)),
			       packet->src_ipaddr.af == AF_INET6 ? "]" : "",
			       packet->src_port,
			       packet->dst_ipaddr.af == AF_INET6 ? "[" : "",
			       inet_ntop(packet->dst_ipaddr.af,
					 &packet->dst_ipaddr.ipaddr,
					 dst_ipaddr, sizeof(dst_ipaddr)),
			       packet->dst_ipaddr.af == AF_INET6 ? "]" : "",
			       packet->dst_port,
#if defined(WITH_UDPFROMTO) && defined(WITH_IFINDEX_RESOLUTION)
			       packet->if_index ? "via " : "",
			       packet->if_index ? fr_ifname_from_ifindex(if_name, packet->if_index) : "",
			       packet->if_index ? " " : "",
#endif
			       packet->data_len);
	}

	if (received) {
		rdebug_pair_list(L_DBG_LVL_1, request, packet->vps, "");
	} else {
		rdebug_proto_pair_list(L_DBG_LVL_1, request, packet->vps, "");
	}
}
static CONF_PARSER performance_config[] = {
	{ FR_CONF_OFFSET("skip_duplicate_checks", PW_TYPE_BOOLEAN, rad_listen_t, nodup) },

	{ FR_CONF_OFFSET("synchronous", PW_TYPE_BOOLEAN, rad_listen_t, synchronous) },

	{ FR_CONF_OFFSET("workers", PW_TYPE_INTEGER, rad_listen_t, workers) },
	CONF_PARSER_TERMINATOR
};


static CONF_PARSER limit_config[] = {
	{ FR_CONF_OFFSET("max_pps", PW_TYPE_INTEGER, listen_socket_t, max_rate) },

#ifdef WITH_TCP
	{ FR_CONF_OFFSET("max_connections", PW_TYPE_INTEGER, listen_socket_t, limit.max_connections), .dflt = "16" },
	{ FR_CONF_OFFSET("lifetime", PW_TYPE_INTEGER, listen_socket_t, limit.lifetime), .dflt = "0" },
	{ FR_CONF_OFFSET("idle_timeout", PW_TYPE_INTEGER, listen_socket_t, limit.idle_timeout), .dflt = STRINGIFY(30) },
#endif
	CONF_PARSER_TERMINATOR
};


#ifdef WITH_TCP
/*
 *	TLS requires child threads to handle the listeners.  Which
 *	means that we need a separate talloc context per child thread.
 *	Which means that we need to manually clean up the child
 *	listeners.  Which means we need to manually track them.
 *
 *	All child thread linking/unlinking is done in the master
 *	thread.  If we care, we can later add a mutex for the parent
 *	listener.
 */
static int listener_cmp(void const *one, void const *two)
{
	if (one < two) return -1;
	if (one > two) return +1;
	return 0;
}

static int listener_unlink(UNUSED void *ctx, UNUSED void *data)
{
	return 2;		/* unlink this node from the tree */
}
#endif


/*
 *	Parse an authentication or accounting socket.
 */
int common_socket_parse(CONF_SECTION *cs, rad_listen_t *this)
{
	int		rcode;
	uint16_t	listen_port;
	uint32_t	recv_buff;
	fr_ipaddr_t	ipaddr;
	listen_socket_t *sock = this->data;
	char const	*section_name = NULL;
	CONF_SECTION	*client_cs, *parentcs;
	CONF_SECTION	*subcs;
	CONF_PAIR	*cp;

	this->cs = cs;

	/*
	 *	Try IPv4 first
	 */
	memset(&ipaddr, 0, sizeof(ipaddr));
	ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_NONE);

	rcode = cf_item_parse(cs, "ipaddr", FR_ITEM_POINTER(PW_TYPE_COMBO_IP_ADDR, &ipaddr), NULL, T_INVALID);
	if (rcode < 0) return -1;
	if (rcode != 0) rcode = cf_item_parse(cs, "ipv4addr",
					      FR_ITEM_POINTER(PW_TYPE_IPV4_ADDR, &ipaddr), NULL, T_INVALID);
	if (rcode < 0) return -1;
	if (rcode != 0) rcode = cf_item_parse(cs, "ipv6addr",
					      FR_ITEM_POINTER(PW_TYPE_IPV6_ADDR, &ipaddr), NULL, T_INVALID);
	if (rcode < 0) return -1;
	/*
	 *	Default to IPv4 INADDR_ANY
	 */
	if (rcode != 0) {
		memset(&ipaddr, 0, sizeof(ipaddr));
		ipaddr.af = INADDR_ANY;
		ipaddr.prefix = 32;
		ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_ANY);
	}

	rcode = cf_item_parse(cs, "port", FR_ITEM_POINTER(PW_TYPE_SHORT, &listen_port), "0", T_BARE_WORD);
	if (rcode < 0) return -1;

	rcode = cf_item_parse(cs, "recv_buff", FR_ITEM_POINTER(PW_TYPE_INTEGER, &recv_buff), "0", T_BARE_WORD);
	if (rcode < 0) return -1;
	if (recv_buff) {
		FR_INTEGER_BOUND_CHECK("recv_buff", recv_buff, >=, 32);
		FR_INTEGER_BOUND_CHECK("recv_buff", recv_buff, <=, INT_MAX);
	}

	sock->proto = IPPROTO_UDP;

	if (cf_pair_find(cs, "proto")) {
#ifndef WITH_TCP
		cf_log_err_cs(cs, "System does not support the TCP protocol.  "
			      "Delete this line from the configuration file");
		return -1;
#else
		char const *proto = NULL;
#  ifdef WITH_TLS
		CONF_SECTION *tls;
#  endif

		rcode = cf_item_parse(cs, "proto", FR_ITEM_POINTER(PW_TYPE_STRING, &proto),
				      "udp", T_DOUBLE_QUOTED_STRING);
		if (rcode < 0) return -1;

		if (!proto || strcmp(proto, "udp") == 0) {
			sock->proto = IPPROTO_UDP;

		} else if (strcmp(proto, "tcp") == 0) {
			sock->proto = IPPROTO_TCP;
		} else {
			cf_log_err_cs(cs, "Unknown proto name \"%s\"", proto);
			return -1;
		}

		/*
		 *	TCP requires a destination IP for sockets.
		 *	UDP doesn't, so it's allowed.
		 */
#  ifdef WITH_PROXY
		if ((this->type == RAD_LISTEN_PROXY) &&
		    (sock->proto != IPPROTO_UDP)) {
			cf_log_err_cs(cs, "Proxy listeners can only listen on proto = udp");
			return -1;
		}
#  endif	/* WITH_PROXY */

#  ifdef WITH_TLS
		tls = cf_section_sub_find(cs, "tls");
		if (tls) {
			/*
			 *	Don't allow TLS configurations for UDP sockets.
			 */
			if (sock->proto != IPPROTO_TCP) {
				cf_log_err_cs(cs, "TLS transport is not available for UDP sockets");
				return -1;
			}

			/*
			 *	If unset, set to default.
			 */
			if (listen_port == 0) listen_port = PW_RADIUS_TLS_PORT;

			this->tls = tls_server_conf_parse(tls);
			if (!this->tls) {
				return -1;
			}

#    ifdef HAVE_PTRHEAD_H
			if (pthread_mutex_init(&sock->mutex, NULL) < 0) {
				rad_assert(0 == 1);
				listen_free(&this);
				return 0;
			}
#    endif

		}
#  else   /* WITH_TLS */
		/*
		 *	Built without TLS.  Disallow it.
		 */
		if (cf_section_sub_find(cs, "tls")) {
			cf_log_err_cs(cs,
				   "TLS transport is not available in this executable");
			return -1;
		}
#  endif  /* WITH_TLS */
#endif    /* WITH_TCP */

		/*
		 *	No "proto" field.  Disallow TLS.
		 */
	} else if (cf_section_sub_find(cs, "tls")) {
		cf_log_err_cs(cs,
			   "TLS transport is not available in this \"listen\" section");
		return -1;
	}

	/*
	 *	Magical tuning methods!
	 */
	subcs = cf_section_sub_find(cs, "performance");
	if (subcs) {
		rcode = cf_section_parse(subcs, this,
					 performance_config);
		if (rcode < 0) return -1;

		if (this->synchronous && sock->max_rate) {
			WARN("Setting 'max_pps' is incompatible with 'synchronous'.  Disabling 'max_pps'");
			sock->max_rate = 0;
		}

		if (!this->synchronous && this->workers) {
			WARN("Setting 'workers' requires 'synchronous'.  Disabling 'workers'");
			this->workers = 0;
		}
	}

	subcs = cf_section_sub_find(cs, "limit");
	if (subcs) {
		rcode = cf_section_parse(subcs, sock,
					 limit_config);
		if (rcode < 0) return -1;

		if (sock->max_rate && ((sock->max_rate < 10) || (sock->max_rate > 1000000))) {
			cf_log_err_cs(cs,
				      "Invalid value for \"max_pps\"");
			return -1;
		}

#ifdef WITH_TCP
		if ((sock->limit.idle_timeout > 0) && (sock->limit.idle_timeout < 5)) {
			WARN("Setting idle_timeout to 5");
			sock->limit.idle_timeout = 5;
		}

		if ((sock->limit.lifetime > 0) && (sock->limit.lifetime < 5)) {
			WARN("Setting lifetime to 5");
			sock->limit.lifetime = 5;
		}

		if ((sock->limit.lifetime > 0) && (sock->limit.idle_timeout > sock->limit.lifetime)) {
			WARN("Setting idle_timeout to 0");
			sock->limit.idle_timeout = 0;
		}

		/*
		 *	Force no duplicate detection for TCP sockets.
		 */
		if (sock->proto == IPPROTO_TCP) {
			this->nodup = true;
		}

	} else {
		sock->limit.max_connections = 60;
		sock->limit.idle_timeout = 30;
		sock->limit.lifetime = 0;
#endif
	}

	sock->my_ipaddr = ipaddr;
	sock->my_port = listen_port;
	sock->recv_buff = recv_buff;

#ifdef WITH_PROXY
	if (check_config) {
		/*
		 *	Until there is a side effects free way of forwarding a
		 *	request to another virtual server, this check is invalid,
		 *	and should be left disabled.
		 */
#  if 0
		if (home_server_find(&sock->my_ipaddr, sock->my_port, sock->proto)) {
				char buffer[128];

				ERROR("We have been asked to listen on %s port %d, which is also listed as a "
				       "home server.  This can create a proxy loop",
				       fr_inet_ntoh(&sock->my_ipaddr, buffer, sizeof(buffer)), sock->my_port);
				return -1;
		}
#  endif
		return 0;	/* don't do anything */
	}
#endif

	/*
	 *	If we can bind to interfaces, do so,
	 *	else don't.
	 */
	cp = cf_pair_find(cs, "interface");
	if (cp) {
		char const *value = cf_pair_value(cp);
		if (!value) {
			cf_log_err_cs(cs,
				   "No interface name given");
			return -1;
		}
		sock->interface = value;
	}

#ifdef WITH_DHCP
	/*
	 *	If we can do broadcasts..
	 */
	cp = cf_pair_find(cs, "broadcast");
	if (cp) {

	/*
	 *	Testing SO_BROADCAST only makes sence if using sockets
	 *	(i.e. if SO_BINDTODEVICE is available).
	 */
#  if defined(SO_BINDTODEVICE) && !defined(SO_BROADCAST)
		cf_log_err_cs(cs,
			   "System does not support broadcast sockets.  Delete this line from the configuration file");
		return -1;
#  else
		if (this->type != RAD_LISTEN_DHCP) {
			cf_log_err_cp(cp,
				   "Broadcast can only be set for DHCP listeners.  Delete this line from the configuration file");
			return -1;
		}

		char const *value = cf_pair_value(cp);
		if (!value) {
			cf_log_err_cs(cs,
				   "No broadcast value given");
			return -1;
		}

		/*
		 *	Hack... whatever happened to cf_section_parse?
		 */
		sock->broadcast = (strcmp(value, "yes") == 0);
#  endif
	}
#endif


#ifdef HAVE_LIBPCAP
	/* Only use libpcap if pcap_type has a value. Otherwise, use socket with SO_BINDTODEVICE */
	if (sock->interface && sock->pcap_type) {
		if (init_pcap(this) < 0) {
			cf_log_err_cs(cs,
				   "Error initializing pcap.");
			return -1;
		}
	} else
#endif
		/*
		 *	And bind it to the port.
		 */
		if (listen_bind(this) < 0) {
			char buffer[128];
			cf_log_err_cs(cs,
				   "Error binding to port for %s port %d",
				   fr_inet_ntoh(&sock->my_ipaddr, buffer, sizeof(buffer)),
				   sock->my_port);
			return -1;
		}


#ifdef WITH_PROXY
	/*
	 *	Proxy sockets don't have clients.
	 */
	if (this->type == RAD_LISTEN_PROXY) return 0;
#endif

	/*
	 *	The more specific configurations are preferred to more
	 *	generic ones.
	 */
	client_cs = NULL;
	parentcs = cf_top_section(cs);
	rcode = cf_item_parse(cs, "clients", FR_ITEM_POINTER(PW_TYPE_STRING, &section_name), NULL, T_INVALID);
	if (rcode < 0) return -1; /* bad string */
	if (rcode == 0) {
		/*
		 *	Explicit list given: use it.
		 */
		client_cs = cf_section_sub_find_name2(parentcs, "clients", section_name);
		if (!client_cs) {
			client_cs = cf_section_find(section_name);
		}
		if (!client_cs) {
			cf_log_err_cs(cs,
				   "Failed to find clients %s {...}",
				   section_name);
			return -1;
		}
	} /* else there was no "clients = " entry. */

	if (!client_cs) {
		CONF_SECTION *server_cs;

		server_cs = cf_section_sub_find_name2(parentcs,
						      "server",
						      this->server);
		/*
		 *	Found a "server foo" section.  If there are clients
		 *	in it, use them.
		 */
		if (server_cs &&
		    (cf_section_sub_find(server_cs, "client") != NULL)) {
			client_cs = server_cs;
		}
	}

	/*
	 *	Still nothing.  Look for global clients.
	 */
	if (!client_cs) client_cs = parentcs;

#ifdef WITH_TLS
	sock->clients = client_list_parse_section(client_cs, (this->tls != NULL));
#else
	sock->clients = client_list_parse_section(client_cs, false);
#endif
	if (!sock->clients) {
		cf_log_err_cs(cs,
			   "Failed to load clients for this listen section");
		return -1;
	}

#ifdef WITH_TCP
	if (sock->proto == IPPROTO_TCP) {
		/*
		 *	Re-write the listener receive function to
		 *	allow us to accept the socket.
		 */
		this->recv = dual_tcp_accept;

		this->children = rbtree_create(this, listener_cmp, NULL, 0);
		if (!this->children) {
			cf_log_err_cs(cs, "Failed to create child list for TCP socket.");
			return -1;
		}
	}
#endif

	return 0;
}

/*
 *	Send an authentication response packet
 */
static int auth_socket_send(rad_listen_t *listener, REQUEST *request)
{
	rad_assert(request->listener == listener);
	rad_assert(listener->send == auth_socket_send);

	if (request->reply->code == 0) return 0;

#ifdef WITH_UDPFROMTO
	/*
	 *	Overwrite the src ip address on the outbound packet
	 *	with the one specified by the client.
	 *	This is useful to work around broken DSR implementations
	 *	and other routing issues.
	 */
	if (request->client->src_ipaddr.af != AF_UNSPEC) {
		request->reply->src_ipaddr = request->client->src_ipaddr;
	}
#endif

	if (rad_send(request->reply, request->packet,
		     request->client->secret) < 0) {
		RERROR("Failed sending reply: %s",
			       fr_strerror());
		return -1;
	}

	return 0;
}


#ifdef WITH_ACCOUNTING
/*
 *	Send an accounting response packet (or not)
 */
static int acct_socket_send(rad_listen_t *listener, REQUEST *request)
{
	rad_assert(request->listener == listener);
	rad_assert(listener->send == acct_socket_send);

	/*
	 *	Accounting reject's are silently dropped.
	 *
	 *	We do it here to avoid polluting the rest of the
	 *	code with this knowledge
	 */
	if (request->reply->code == 0) return 0;

#  ifdef WITH_UDPFROMTO
	/*
	 *	Overwrite the src ip address on the outbound packet
	 *	with the one specified by the client.
	 *	This is useful to work around broken DSR implementations
	 *	and other routing issues.
	 */
	if (request->client->src_ipaddr.af != AF_UNSPEC) {
		request->reply->src_ipaddr = request->client->src_ipaddr;
	}
#  endif

	if (rad_send(request->reply, request->packet,
		     request->client->secret) < 0) {
		RERROR("Failed sending reply: %s",
			       fr_strerror());
		return -1;
	}

	return 0;
}
#endif

#ifdef WITH_PROXY
/*
 *	Send a packet to a home server.
 *
 *	FIXME: have different code for proxy auth & acct!
 */
static int proxy_socket_send(rad_listen_t *listener, REQUEST *request)
{
	rad_assert(request->proxy_listener == listener);
	rad_assert(listener->send == proxy_socket_send);

	if (rad_send(request->proxy, NULL,
		     request->home_server->secret) < 0) {
		RERROR("Failed sending proxied request: %s",
			       fr_strerror());
		return -1;
	}

	return 0;
}
#endif

#ifdef WITH_STATS
/*
 *	Check if an incoming request is "ok"
 *
 *	It takes packets, not requests.  It sees if the packet looks
 *	OK.  If so, it does a number of sanity checks on it.
  */
static int stats_socket_recv(rad_listen_t *listener)
{
	ssize_t		rcode;
	int		code;
	uint16_t	src_port;
	RADIUS_PACKET	*packet;
	RADCLIENT	*client = NULL;
	fr_ipaddr_t	src_ipaddr;

	rcode = rad_recv_header(listener->fd, &src_ipaddr, &src_port, &code);
	if (rcode < 0) return 0;

	FR_STATS_INC(auth, total_requests);

	if (rcode < 20) {	/* RADIUS_HDR_LEN */
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		FR_STATS_INC(auth, total_malformed_requests);
		return 0;
	}

	client = client_listener_find(listener, &src_ipaddr, src_port);
	if (!client) {
		rad_recv_discard(listener->fd);
		FR_STATS_INC(auth, total_invalid_requests);
		return 0;
	}

	FR_STATS_TYPE_INC(client->auth.total_requests);

	/*
	 *	We only understand Status-Server on this socket.
	 */
	if (code != PW_CODE_STATUS_SERVER) {
		DEBUG("Ignoring packet code %d sent to Status-Server port",
		      code);
		rad_recv_discard(listener->fd);
		FR_STATS_INC(auth, total_unknown_types);
		return 0;
	}

	/*
	 *	Now that we've sanity checked everything, receive the
	 *	packet.
	 */
	packet = rad_recv(NULL, listener->fd, 1); /* require message authenticator */
	if (!packet) {
		FR_STATS_INC(auth, total_malformed_requests);
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		return 0;
	}

	if (!request_receive(NULL, listener, packet, client, rad_status_server)) {
		FR_STATS_INC(auth, total_packets_dropped);
		rad_free(&packet);
		return 0;
	}

	return 1;
}
#endif


/*
 *	Check if an incoming request is "ok"
 *
 *	It takes packets, not requests.  It sees if the packet looks
 *	OK.  If so, it does a number of sanity checks on it.
  */
static int auth_socket_recv(rad_listen_t *listener)
{
	ssize_t		rcode;
	int		code;
	uint16_t	src_port;
	RADIUS_PACKET	*packet;
	RAD_REQUEST_FUNP fun = NULL;
	RADCLIENT	*client = NULL;
	fr_ipaddr_t	src_ipaddr;
	TALLOC_CTX	*ctx;

	rcode = rad_recv_header(listener->fd, &src_ipaddr, &src_port, &code);
	if (rcode < 0) return 0;

	FR_STATS_INC(auth, total_requests);

	if (rcode < 20) {	/* RADIUS_HDR_LEN */
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		FR_STATS_INC(auth, total_malformed_requests);
		return 0;
	}

	client = client_listener_find(listener, &src_ipaddr, src_port);
	if (!client) {
		rad_recv_discard(listener->fd);
		FR_STATS_INC(auth, total_invalid_requests);
		return 0;
	}

	FR_STATS_TYPE_INC(client->auth.total_requests);

	/*
	 *	Some sanity checks, based on the packet code.
	 */
	switch (code) {
	case PW_CODE_ACCESS_REQUEST:
		fun = rad_authenticate;
		break;

	case PW_CODE_STATUS_SERVER:
		if (!main_config.status_server) {
			rad_recv_discard(listener->fd);
			FR_STATS_INC(auth, total_unknown_types);
			WARN("Ignoring Status-Server request due to security configuration");
			return 0;
		}
		fun = rad_status_server;
		break;

	default:
		rad_recv_discard(listener->fd);
		FR_STATS_INC(auth, total_unknown_types);

		if (DEBUG_ENABLED) ERROR("Receive - Invalid packet code %d sent to authentication port from "
					 "client %s port %d", code, client->shortname, src_port);
		return 0;
	} /* switch over packet types */

	ctx = talloc_pool(NULL, main_config.talloc_pool_size);
	if (!ctx) {
		rad_recv_discard(listener->fd);
		FR_STATS_INC(auth, total_packets_dropped);
		return 0;
	}
	talloc_set_name_const(ctx, "auth_listener_pool");

	/*
	 *	Now that we've sanity checked everything, receive the
	 *	packet.
	 */
	packet = rad_recv(ctx, listener->fd, client->message_authenticator);
	if (!packet) {
		FR_STATS_INC(auth, total_malformed_requests);
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		talloc_free(ctx);
		return 0;
	}

#if defined(__APPLE__) && defined(WITH_UDPFROMTO)
	/*
	 *	This is a NICE Mac OSX bug.  Create an interface with
	 *	two IP address, and then configure one listener for
	 *	each IP address.  Send thousands of packets to one
	 *	address, and some will show up on the OTHER socket.
	 *
	 *	This hack works ONLY if the clients are global.  If
	 *	each listener has the same client IP, but with
	 *	different secrets, then it will fail the rad_recv()
	 *	check above, and there's nothing you can do.
	 */
	{
		listen_socket_t *sock = listener->data;
		rad_listen_t *other;

		other = listener_find_byipaddr(&packet->dst_ipaddr,
					       packet->dst_port, sock->proto);
		if (other) listener = other;
	}
#endif

	if (!request_receive(ctx, listener, packet, client, fun)) {
		FR_STATS_INC(auth, total_packets_dropped);
		talloc_free(ctx);
		return 0;
	}

	return 1;
}


#ifdef WITH_ACCOUNTING
/*
 *	Receive packets from an accounting socket
 */
static int acct_socket_recv(rad_listen_t *listener)
{
	ssize_t		rcode;
	int		code;
	uint16_t	src_port;
	RADIUS_PACKET	*packet;
	RAD_REQUEST_FUNP fun = NULL;
	RADCLIENT	*client = NULL;
	fr_ipaddr_t	src_ipaddr;
	TALLOC_CTX	*ctx;

	rcode = rad_recv_header(listener->fd, &src_ipaddr, &src_port, &code);
	if (rcode < 0) return 0;

	FR_STATS_INC(acct, total_requests);

	if (rcode < 20) {	/* RADIUS_HDR_LEN */
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		FR_STATS_INC(acct, total_malformed_requests);
		return 0;
	}

	if ((client = client_listener_find(listener,
					   &src_ipaddr, src_port)) == NULL) {
		rad_recv_discard(listener->fd);
		FR_STATS_INC(acct, total_invalid_requests);
		return 0;
	}

	FR_STATS_TYPE_INC(client->acct.total_requests);

	/*
	 *	Some sanity checks, based on the packet code.
	 */
	switch (code) {
	case PW_CODE_ACCOUNTING_REQUEST:
		fun = rad_accounting;
		break;

	case PW_CODE_STATUS_SERVER:
		if (!main_config.status_server) {
			rad_recv_discard(listener->fd);
			FR_STATS_INC(acct, total_unknown_types);

			WARN("Ignoring Status-Server request due to security configuration");
			return 0;
		}
		fun = rad_status_server;
		break;

	default:
		rad_recv_discard(listener->fd);
		FR_STATS_INC(acct, total_unknown_types);

		DEBUG("Invalid packet code %d sent to a accounting port from client %s port %d : IGNORED",
		      code, client->shortname, src_port);
		return 0;
	} /* switch over packet types */

	ctx = talloc_pool(NULL, main_config.talloc_pool_size);
	if (!ctx) {
		rad_recv_discard(listener->fd);
		FR_STATS_INC(acct, total_packets_dropped);
		return 0;
	}
	talloc_set_name_const(ctx, "acct_listener_pool");

	/*
	 *	Now that we've sanity checked everything, receive the
	 *	packet.
	 */
	packet = rad_recv(ctx, listener->fd, 0);
	if (!packet) {
		FR_STATS_INC(acct, total_malformed_requests);
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		talloc_free(ctx);
		return 0;
	}

	/*
	 *	There can be no duplicate accounting packets.
	 */
	if (!request_receive(ctx, listener, packet, client, fun)) {
		FR_STATS_INC(acct, total_packets_dropped);
		rad_free(&packet);
		talloc_free(ctx);
		return 0;
	}

	return 1;
}
#endif


#ifdef WITH_COA
static int do_proxy(REQUEST *request)
{
	VALUE_PAIR *vp;

	if (request->in_proxy_hash ||
	    (request->proxy_reply && (request->proxy_reply->code != 0))) {
		return 0;
	}

	vp = fr_pair_find_by_num(request->config, PW_HOME_SERVER_POOL, 0, TAG_ANY);

	if (vp) {
		if (!home_pool_byname(vp->vp_strvalue, HOME_TYPE_COA)) {
			REDEBUG2("Cannot proxy to unknown pool %s",
				 vp->vp_strvalue);
			return -1;
		}

		return 1;
	}

	/*
	 *	We have a destination IP address.  It will (later) proxied.
	 */
	vp = fr_pair_find_by_num(request->config, PW_PACKET_DST_IP_ADDRESS, 0, TAG_ANY);
	if (!vp) vp = fr_pair_find_by_num(request->config, PW_PACKET_DST_IPV6_ADDRESS, 0, TAG_ANY);

	if (!vp) return 0;

	return 1;
}

/*
 *	Receive a CoA packet.
 */
int rad_coa_recv(REQUEST *request)
{
	int rcode = RLM_MODULE_OK;
	int ack, nak;
	int proxy_status;
	VALUE_PAIR *vp;

	/*
	 *	Get the correct response
	 */
	switch (request->packet->code) {
	case PW_CODE_COA_REQUEST:
		ack = PW_CODE_COA_ACK;
		nak = PW_CODE_COA_NAK;
		break;

	case PW_CODE_DISCONNECT_REQUEST:
		ack = PW_CODE_DISCONNECT_ACK;
		nak = PW_CODE_DISCONNECT_NAK;
		break;

	default:		/* shouldn't happen */
		return RLM_MODULE_FAIL;
	}

#  ifdef WITH_PROXY
#    define WAS_PROXIED (request->proxy)
#  else
#    define WAS_PROXIED (0)
#  endif

	if (!WAS_PROXIED) {
		/*
		 *	RFC 5176 Section 3.3.  If we have a CoA-Request
		 *	with Service-Type = Authorize-Only, it MUST
		 *	have a State attribute in it.
		 */
		vp = fr_pair_find_by_num(request->packet->vps, PW_SERVICE_TYPE, 0, TAG_ANY);
		if (request->packet->code == PW_CODE_COA_REQUEST) {
			if (vp && (vp->vp_integer == PW_AUTHORIZE_ONLY)) {
				vp = fr_pair_find_by_num(request->packet->vps, PW_STATE, 0, TAG_ANY);
				if (!vp || (vp->vp_length == 0)) {
					REDEBUG("CoA-Request with Service-Type = Authorize-Only MUST "
						"contain a State attribute");
					request->reply->code = PW_CODE_COA_NAK;
					return RLM_MODULE_FAIL;
				}
			}
		} else if (vp) {
			/*
			 *	RFC 5176, Section 3.2.
			 */
			REDEBUG("Disconnect-Request MUST NOT contain a Service-Type attribute");
			request->reply->code = PW_CODE_DISCONNECT_NAK;
			return RLM_MODULE_FAIL;
		}

		rcode = process_recv_coa(0, request);
		switch (rcode) {
		case RLM_MODULE_FAIL:
		case RLM_MODULE_INVALID:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_USERLOCK:
		default:
			request->reply->code = nak;
			break;

		case RLM_MODULE_HANDLED:
			return rcode;

		case RLM_MODULE_NOOP:
		case RLM_MODULE_NOTFOUND:
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
			proxy_status = do_proxy(request);
			if (proxy_status == 1) return RLM_MODULE_OK;

			if (proxy_status < 0) {
				request->reply->code = nak;
			} else {
				request->reply->code = ack;
			}
			break;
		}

	}

#  ifdef WITH_PROXY
	else if (request->proxy_reply) {
		/*
		 *	Start the reply code with the proxy reply
		 *	code.
		 */
		request->reply->code = request->proxy_reply->code;
	}
#  endif

	/*
	 *	Copy State from the request to the reply.
	 *	See RFC 5176 Section 3.3.
	 */
	vp = fr_pair_list_copy_by_num(request->reply, request->packet->vps, PW_STATE, 0, TAG_ANY);
	if (vp) fr_pair_add(&request->reply->vps, vp);

	/*
	 *	We may want to over-ride the reply.
	 */
	if (request->reply->code) {
		rcode = process_send_coa(0, request);
		switch (rcode) {
			/*
			 *	We need to send CoA-NAK back if Service-Type
			 *	is Authorize-Only.  Rely on the user's policy
			 *	to do that.  We're not a real NAS, so this
			 *	restriction doesn't (ahem) apply to us.
			 */
		case RLM_MODULE_FAIL:
		case RLM_MODULE_INVALID:
		case RLM_MODULE_REJECT:
		case RLM_MODULE_USERLOCK:
		default:
			/*
			 *	Over-ride an ACK with a NAK
			 */
			request->reply->code = nak;
			break;

		case RLM_MODULE_HANDLED:
			return rcode;

		case RLM_MODULE_NOOP:
		case RLM_MODULE_NOTFOUND:
		case RLM_MODULE_OK:
		case RLM_MODULE_UPDATED:
			/*
			 *	Do NOT over-ride a previously set value.
			 *	Otherwise an "ok" here will re-write a
			 *	NAK to an ACK.
			 */
			if (request->reply->code == 0) {
				request->reply->code = ack;
			}
			break;
		}
	}

	return RLM_MODULE_OK;
}


/*
 *	Check if an incoming request is "ok"
 *
 *	It takes packets, not requests.  It sees if the packet looks
 *	OK.  If so, it does a number of sanity checks on it.
  */
static int coa_socket_recv(rad_listen_t *listener)
{
	ssize_t		rcode;
	int		code;
	uint16_t	src_port;
	RADIUS_PACKET	*packet;
	RAD_REQUEST_FUNP fun = NULL;
	RADCLIENT	*client = NULL;
	fr_ipaddr_t	src_ipaddr;
	TALLOC_CTX	*ctx;

	rcode = rad_recv_header(listener->fd, &src_ipaddr, &src_port, &code);
	if (rcode < 0) return 0;

	if (rcode < 20) {	/* RADIUS_HDR_LEN */
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		FR_STATS_INC(coa, total_malformed_requests);
		return 0;
	}

	if ((client = client_listener_find(listener,
					   &src_ipaddr, src_port)) == NULL) {
		rad_recv_discard(listener->fd);
		FR_STATS_INC(coa, total_requests);
		FR_STATS_INC(coa, total_invalid_requests);
		return 0;
	}

	/*
	 *	Some sanity checks, based on the packet code.
	 */
	switch (code) {
	case PW_CODE_COA_REQUEST:
		FR_STATS_INC(coa, total_requests);
		fun = rad_coa_recv;
		break;

	case PW_CODE_DISCONNECT_REQUEST:
		FR_STATS_INC(dsc, total_requests);
		fun = rad_coa_recv;
		break;

	default:
		rad_recv_discard(listener->fd);
		FR_STATS_INC(coa, total_unknown_types);
		DEBUG("Invalid packet code %d sent to coa port from client %s port %d : IGNORED",
		      code, client->shortname, src_port);
		return 0;
	} /* switch over packet types */

	ctx = talloc_pool(NULL, main_config.talloc_pool_size);
	if (!ctx) {
		rad_recv_discard(listener->fd);
		FR_STATS_INC(coa, total_packets_dropped);
		return 0;
	}
	talloc_set_name_const(ctx, "coa_socket_recv_pool");

	/*
	 *	Now that we've sanity checked everything, receive the
	 *	packet.
	 */
	packet = rad_recv(ctx, listener->fd, client->message_authenticator);
	if (!packet) {
		FR_STATS_INC(coa, total_malformed_requests);
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		talloc_free(ctx);
		return 0;
	}

	if (!request_receive(ctx, listener, packet, client, fun)) {
		FR_STATS_INC(coa, total_packets_dropped);
		rad_free(&packet);
		talloc_free(ctx);
		return 0;
	}

	return 1;
}
#endif

#ifdef WITH_PROXY
/*
 *	Recieve packets from a proxy socket.
 */
static int proxy_socket_recv(rad_listen_t *listener)
{
	RADIUS_PACKET	*packet;
#  ifdef WITH_TCP
	listen_socket_t *sock;
#  endif
	char		buffer[128];

	packet = rad_recv(NULL, listener->fd, 0);
	if (!packet) {
		if (DEBUG_ENABLED) ERROR("Receive - %s", fr_strerror());
		return 0;
	}

	switch (packet->code) {
	case PW_CODE_ACCESS_ACCEPT:
	case PW_CODE_ACCESS_CHALLENGE:
	case PW_CODE_ACCESS_REJECT:
		break;

#  ifdef WITH_ACCOUNTING
	case PW_CODE_ACCOUNTING_RESPONSE:
		break;
#  endif

#  ifdef WITH_COA
	case PW_CODE_DISCONNECT_ACK:
	case PW_CODE_DISCONNECT_NAK:
	case PW_CODE_COA_ACK:
	case PW_CODE_COA_NAK:
		break;
#  endif

	default:
		/*
		 *	FIXME: Update MIB for packet types?
		 */
		ERROR("Invalid packet code %d sent to a proxy port from home server %s port %d - ID %d : IGNORED",
		      packet->code,
		      fr_inet_ntoh(&packet->src_ipaddr, buffer, sizeof(buffer)),
		      packet->src_port, packet->id);
#  ifdef WITH_STATS
		listener->stats.total_unknown_types++;
#  endif
		rad_free(&packet);
		return 0;
	}

#  ifdef WITH_TCP
	sock = listener->data;
	packet->proto = sock->proto;
#  endif

	if (!request_proxy_reply(packet)) {
#  ifdef WITH_STATS
		listener->stats.total_packets_dropped++;
#  endif
		rad_free(&packet);
		return 0;
	}

	return 1;
}

#  ifdef WITH_TCP
/*
 *	Recieve packets from a proxy socket.
 */
static int proxy_socket_tcp_recv(rad_listen_t *listener)
{
	RADIUS_PACKET	*packet;
	listen_socket_t	*sock = listener->data;
	char		buffer[128];

	if (listener->status != RAD_LISTEN_STATUS_KNOWN) return 0;

	packet = fr_tcp_recv(listener->fd, 0);
	if (!packet) {
		listener->status = RAD_LISTEN_STATUS_EOL;
		radius_update_listener(listener);
		return 0;
	}

	/*
	 *	FIXME: Client MIB updates?
	 */
	switch (packet->code) {
	case PW_CODE_ACCESS_ACCEPT:
	case PW_CODE_ACCESS_CHALLENGE:
	case PW_CODE_ACCESS_REJECT:
		break;

#    ifdef WITH_ACCOUNTING
	case PW_CODE_ACCOUNTING_RESPONSE:
		break;
#    endif

	default:
		/*
		 *	FIXME: Update MIB for packet types?
		 */
		ERROR("Invalid packet code %d sent to a proxy port "
		       "from home server %s port %d - ID %d : IGNORED",
		       packet->code,
		       fr_inet_ntoh(&packet->src_ipaddr, buffer, sizeof(buffer)),
		       packet->src_port, packet->id);
		rad_free(&packet);
		return 0;
	}

	packet->src_ipaddr = sock->other_ipaddr;
	packet->src_port = sock->other_port;
	packet->dst_ipaddr = sock->my_ipaddr;
	packet->dst_port = sock->my_port;

	/*
	 *	FIXME: Have it return an indication of packets that
	 *	are OK to ignore (dups, too late), versus ones that
	 *	aren't OK to ignore (unknown response, spoofed, etc.)
	 *
	 *	Close the socket on bad packets...
	 */
	if (!request_proxy_reply(packet)) {
		rad_free(&packet);
		return 0;
	}

	sock->opened = sock->last_packet = time(NULL);

	return 1;
}
#  endif
#endif


static int client_socket_encode(UNUSED rad_listen_t *listener, REQUEST *request)
{
	if (!request->reply->code) return 0;

	if (rad_encode(request->reply, request->packet, request->client->secret) < 0) {
		RERROR("Failed encoding packet: %s", fr_strerror());

		return -1;
	}

	if (rad_sign(request->reply, request->packet, request->client->secret) < 0) {
		RERROR("Failed signing packet: %s", fr_strerror());

		return -1;
	}

	return 0;
}


static int client_socket_decode(UNUSED rad_listen_t *listener, REQUEST *request)
{
#ifdef WITH_TLS
	listen_socket_t *sock;
#endif

	if (rad_verify(request->packet, NULL,
		       request->client->secret) < 0) {
		return -1;
	}

#ifdef WITH_TLS
	sock = request->listener->data;
	rad_assert(sock != NULL);

	/*
	 *	FIXME: Add the rest of the TLS parameters, too?  But
	 *	how do we separate EAP-TLS parameters from RADIUS/TLS
	 *	parameters?
	 */
	if (sock->ssn && sock->ssn->ssl) {
#  ifdef PSK_MAX_IDENTITY_LEN
		const char *identity = SSL_get_psk_identity(sock->ssn->ssl);
		if (identity) {
			RDEBUG("Retrieved psk identity: %s", identity);
			pair_make_request("TLS-PSK-Identity", identity, T_OP_SET);
		}
#  endif
	}
#endif

	return rad_decode(request->packet, NULL,
			  request->client->secret);
}

#ifdef WITH_PROXY
static int proxy_socket_encode(UNUSED rad_listen_t *listener, REQUEST *request)
{
	if (rad_encode(request->proxy, NULL, request->home_server->secret) < 0) {
		RERROR("Failed encoding proxied packet: %s", fr_strerror());

		return -1;
	}

	if (rad_sign(request->proxy, NULL, request->home_server->secret) < 0) {
		RERROR("Failed signing proxied packet: %s", fr_strerror());

		return -1;
	}

	return 0;
}


static int proxy_socket_decode(UNUSED rad_listen_t *listener, REQUEST *request)
{
	/*
	 *	rad_verify is run in event.c, received_proxy_response()
	 */

	return rad_decode(request->proxy_reply, request->proxy,
			   request->home_server->secret);
}
#endif

#include "command.c"

#define NO_LISTENER { .name = "undefined", }

/*
 *	Handle up to 256 different protocols.
 */
static fr_protocol_t master_listen[MAX_LISTENER] = {
#ifdef WITH_STATS
	{ RLM_MODULE_INIT, "status", sizeof(listen_socket_t), NULL,
	  common_socket_parse, NULL,
	  stats_socket_recv, auth_socket_send,
	  common_socket_print, common_packet_debug, client_socket_encode, client_socket_decode },
#else
	NO_LISTENER,
#endif

#ifdef WITH_PROXY
	/* proxying */
	{ RLM_MODULE_INIT, "proxy", sizeof(listen_socket_t), NULL,
	  common_socket_parse, common_socket_free,
	  proxy_socket_recv, proxy_socket_send,
	  common_socket_print, common_packet_debug, proxy_socket_encode, proxy_socket_decode },
#else
	NO_LISTENER,
#endif

	/* authentication */
	{ RLM_MODULE_INIT, "auth", sizeof(listen_socket_t), NULL,
	  common_socket_parse, common_socket_free,
	  auth_socket_recv, auth_socket_send,
	  common_socket_print, common_packet_debug, client_socket_encode, client_socket_decode },

#ifdef WITH_ACCOUNTING
	/* accounting */
	{ RLM_MODULE_INIT, "acct", sizeof(listen_socket_t), NULL,
	  common_socket_parse, common_socket_free,
	  acct_socket_recv, acct_socket_send,
	  common_socket_print, common_packet_debug, client_socket_encode, client_socket_decode},
#else
	NO_LISTENER,
#endif

#ifdef WITH_DETAIL
	/* detail */
	{ RLM_MODULE_INIT, "detail", sizeof(listen_detail_t), NULL,
	  detail_parse, detail_free,
	  detail_recv, detail_send,
	  detail_print, common_packet_debug, detail_encode, detail_decode },
#else
	NO_LISTENER,
#endif

	NO_LISTENER,		/* vmps */

	NO_LISTENER,		/* dhcp */

#ifdef WITH_COMMAND_SOCKET
	/* TCP command socket */
	{ RLM_MODULE_INIT, "control", sizeof(fr_command_socket_t), NULL,
	  command_socket_parse, command_socket_free,
	  command_domain_accept, command_domain_send,
	  command_socket_print, common_packet_debug, command_socket_encode, command_socket_decode },
#else
	NO_LISTENER,
#endif

#ifdef WITH_COA
	/* Change of Authorization */
	{ RLM_MODULE_INIT, "coa", sizeof(listen_socket_t), NULL,
	  common_socket_parse, NULL,
	  coa_socket_recv, auth_socket_send, /* CoA packets are same as auth */
	  common_socket_print, common_packet_debug, client_socket_encode, client_socket_decode },
#else
	NO_LISTENER,
#endif

	NO_LISTENER		/* bfd */
};


#ifdef HAVE_LIBPCAP
/** Initialize PCAP library based on listen section
 *
 * @param this listen section
 * @return
 *	- 0 if successful
 *	- -1 if failed
 */
static int init_pcap(rad_listen_t *this)
{
	listen_socket_t *sock = this->data;
	char const * pcap_filter;

	sock->pcap = fr_pcap_init(this, sock->interface, sock->pcap_type);

	if (!sock->pcap) {
		ERROR("Failed creating pcap for interface %s", sock->interface);
		return -1;
	}

	if (check_config) return 0;

	rad_suid_up();
	if (fr_pcap_open(sock->pcap) < 0) {
		ERROR("Failed opening interface %s: %s", sock->interface, fr_strerror());
		return -1;
	}
	rad_suid_down();

	pcap_filter = sock->pcap_filter_builder(this);

	if (!pcap_filter) {
		ERROR("Failed building filter for interface %s: %s",
			sock->interface, fr_strerror());
		return -1;
	}

	if (fr_pcap_apply_filter(sock->pcap, pcap_filter) < 0) {
		ERROR("Failed setting filter for interface %s: %s",
			sock->interface, fr_strerror());
		return -1;
	} else {
		DEBUG("Using PCAP filter '%s'", pcap_filter);
	}

	this->fd = sock->pcap->fd;

	return 0;
}
#endif


/*
 *	Binds a listener to a socket.
 */
static int listen_bind(rad_listen_t *this)
{
	int			rcode;
	struct sockaddr_storage	salocal;
	socklen_t		salen;
	listen_socket_t		*sock = this->data;
#ifndef WITH_TCP
#  define proto_for_port "udp"
#  define sock_type SOCK_DGRAM
#else
	char const		*proto_for_port = "udp";
	int			sock_type = SOCK_DGRAM;

	if (sock->proto == IPPROTO_TCP) {
#  ifdef WITH_VMPS
		if (this->type == RAD_LISTEN_VQP) {
			ERROR("VQP does not support TCP transport");
			return -1;
		}
#  endif

		proto_for_port = "tcp";
		sock_type = SOCK_STREAM;
	}
#endif

	/*
	 *	If the port is zero, then it means the appropriate
	 *	thing from /etc/services.
	 */
	if (sock->my_port == 0) {
		struct servent	*svp;

		switch (this->type) {
		case RAD_LISTEN_AUTH:
			svp = getservbyname("radius", proto_for_port);
			if (svp != NULL) {
				sock->my_port = ntohs(svp->s_port);
			} else {
				sock->my_port = PW_AUTH_UDP_PORT;
			}
			break;

#ifdef WITH_ACCOUNTING
		case RAD_LISTEN_ACCT:
			svp = getservbyname("radacct", proto_for_port);
			if (svp != NULL) {
				sock->my_port = ntohs(svp->s_port);
			} else {
				sock->my_port = PW_ACCT_UDP_PORT;
			}
			break;
#endif

#ifdef WITH_PROXY
		case RAD_LISTEN_PROXY:
			/* leave it at zero */
			break;
#endif

#ifdef WITH_VMPS
		case RAD_LISTEN_VQP:
			sock->my_port = 1589;
			break;
#endif

#ifdef WITH_COMMAND_SOCKET
		case RAD_LISTEN_COMMAND:
			sock->my_port = PW_RADMIN_PORT;
			break;
#endif

#ifdef WITH_COA
		case RAD_LISTEN_COA:
			svp = getservbyname("radius-dynauth", "udp");
			if (svp != NULL) {
				sock->my_port = ntohs(svp->s_port);
			} else {
				sock->my_port = PW_COA_UDP_PORT;
			}
			break;
#endif

#ifdef WITH_DHCP
		case RAD_LISTEN_DHCP:
			svp = getservbyname ("bootps", "udp");
			if (svp != NULL) {
				sock->my_port = ntohs(svp->s_port);
			} else {
				sock->my_port = 67;
			}
			break;
#endif

		default:
			WARN("Internal sanity check failed in binding to socket.  Ignoring problem");
			return -1;
		}
	}

	/*
	 *	Don't open sockets if we're checking the config.
	 */
	if (check_config) {
		this->fd = -1;
		return 0;
	}

	rad_assert(sock->my_ipaddr.af);

	DEBUG4("[FD XX] Opening socket -- socket(%s, %s, 0)",
	       fr_int2str(fr_net_af_table, sock->my_ipaddr.af, "<UNKNOWN>"),
	       fr_int2str(fr_net_sock_type_table, sock_type, "<UNKNOWN>"));

	/*
	 *	Copy fr_socket() here, as we may need to bind to a device.
	 */
	this->fd = socket(sock->my_ipaddr.af, sock_type, sock->proto);
	if (this->fd < 0) {
		char buffer[256];

		this->print(this, buffer, sizeof(buffer));

		ERROR("Failed opening %s: %s", buffer, fr_syserror(errno));
		return -1;
	}

#ifdef FD_CLOEXEC
	/*
	 *	We don't want child processes inheriting these
	 *	file descriptors.
	 */
	rcode = fcntl(this->fd, F_GETFD);
	if (rcode >= 0) {
		DEBUG4("[FD %i] Preventing inheritance -- fcntl(%i, F_SETFD, %i | FD_CLOEXEC)",
		       this->fd, this->fd, rcode);
		if (fcntl(this->fd, F_SETFD, rcode | FD_CLOEXEC) < 0) {
			close(this->fd);
			ERROR("Failed setting close on exec: %s", fr_syserror(errno));
			return -1;
		}
	}
#endif

	/*
	 *	Set the receive buffer size
	 */
	if (sock->recv_buff) {
		DEBUG4("[FD %i] Setting recv_buff -- setsockopt(%i, SOL_SOCKET, SO_RCVBUF, %i, %zu)", this->fd,
		       this->fd, sock->recv_buff, sizeof(int));
		if (setsockopt(this->fd, SOL_SOCKET, SO_RCVBUF, (int *)&sock->recv_buff, sizeof(int)) < 0) {
			close(this->fd);
			ERROR("Failed setting receive buffer size: %s", fr_syserror(errno));
			return -1;
		}
	}

	/*
	 *	Bind to a device BEFORE touching IP addresses.
	 */
	if (sock->interface) {
#ifdef SO_BINDTODEVICE
		struct ifreq ifreq;

		memset(&ifreq, 0, sizeof(ifreq));
		strlcpy(ifreq.ifr_name, sock->interface, sizeof(ifreq.ifr_name));

		DEBUG4("[FD %i] Binding to interface %s -- setsockopt(%i, SOL_SOCKET, SO_BINDTODEVICE, %p, %zu)",
		       this->fd, sock->interface, this->fd, &ifreq, sizeof(ifreq));

		rad_suid_up();
		rcode = setsockopt(this->fd, SOL_SOCKET, SO_BINDTODEVICE, (char *)&ifreq, sizeof(ifreq));
		rad_suid_down();
		if (rcode < 0) {
			close(this->fd);
			ERROR("Failed binding to interface %s: %s", sock->interface, fr_syserror(errno));
			return -1;
		} /* else it worked. */
#else
#  ifdef HAVE_STRUCT_SOCKADDR_IN6
#  ifdef HAVE_NET_IF_H
		/*
		 *	Odds are that any system supporting "bind to
		 *	device" also supports IPv6, so this next bit
		 *	isn't necessary.  But it's here for
		 *	completeness.
		 *
		 *	If we're doing IPv6, and the scope hasn't yet
		 *	been defined, set the scope to the scope of
		 *	the interface.
		 */
		if (sock->my_ipaddr.af == AF_INET6) {
			if (sock->my_ipaddr.scope == 0) {
				sock->my_ipaddr.scope = if_nametoindex(sock->interface);
				DEBUG4("[FD %i] IPv6 scope resolves to %u", this->fd, sock->my_ipaddr.scope);
				if (sock->my_ipaddr.scope == 0) {
					close(this->fd);
					ERROR("Failed finding interface %s: %s", sock->interface, fr_syserror(errno));
					return -1;
				}
			} /* else scope was defined: we're OK. */
		} else
#  endif
#endif
				/*
				 *	IPv4: no link local addresses,
				 *	and no bind to device.
				 */
		{
			close(this->fd);
			ERROR("Failed binding to interface %s: \"bind to device\" is unsupported", sock->interface);
			return -1;
		}
#endif
	}

#if defined(WITH_TCP) || defined(WITH_DHCP)
	if ((sock->proto == IPPROTO_TCP) || (this->type == RAD_LISTEN_DHCP)) {
		int on = 1;

		DEBUG4("[FD %i] Enabling SO_REUSEADDR -- setsockopt(%i, SOL_SOCKET, SO_REUSEADDR, %p (%i), %zu)",
		       this->fd, this->fd, &on, on, sizeof(on));
		if (setsockopt(this->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
			close(this->fd);
			ERROR("Failed to reuse address: %s", fr_syserror(errno));
			return -1;
		}
	}
#endif

#if defined(WITH_TCP) && defined(WITH_UDPFROMTO)
	else			/* UDP sockets get UDPfromto */
#endif

#ifdef WITH_UDPFROMTO
	DEBUG4("[FD %i] Enabling IPV6_RECVPKTINFO/IP_PKTINFO -- udpfromto_init(%i)", this->fd, this->fd);
	/*
	 *	Initialize udpfromto for all sockets.
	 */
	if (udpfromto_init(this->fd) != 0) {
		ERROR("Failed initializing udpfromto: %s", fr_syserror(errno));
		close(this->fd);
		return -1;
	}
#endif

	/*
	 *	Set up sockaddr stuff.
	 */
	if (!fr_ipaddr_to_sockaddr(&sock->my_ipaddr, sock->my_port, &salocal, &salen)) {
		close(this->fd);
		return -1;
	}

#ifdef HAVE_STRUCT_SOCKADDR_IN6
	if (sock->my_ipaddr.af == AF_INET6) {
		/*
		 *	Listening on '::' does NOT get you IPv4 to
		 *	IPv6 mapping.  You've got to listen on an IPv4
		 *	address, too.  This makes the rest of the server
		 *	design a little simpler.
		 */
#  ifdef IPV6_V6ONLY

		if (IN6_IS_ADDR_UNSPECIFIED(&sock->my_ipaddr.ipaddr.ip6addr)) {
			int on = 1;

			if (setsockopt(this->fd, IPPROTO_IPV6, IPV6_V6ONLY,
				       (char *)&on, sizeof(on)) < 0) {
				ERROR("Failed setting socket to IPv6 only: %s", fr_syserror(errno));

				close(this->fd);
				return -1;
			}
		}
#  endif /* IPV6_V6ONLY */
	}
#endif /* HAVE_STRUCT_SOCKADDR_IN6 */

	if (sock->my_ipaddr.af == AF_INET) {
#if (defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DONT)) || defined(IP_DONTFRAG)
		int flag;
#endif

#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DONT)

		/*
		 *	Disable PMTU discovery.  On Linux, this
		 *	also makes sure that the "don't fragment"
		 *	flag is zero.
		 */
		flag = IP_PMTUDISC_DONT;

		DEBUG4("[FD %i] Disabling PMTU discovery -- setsockopt(%i, IPPROTO_IP, IP_MTU_DISCOVER, "
		       "%p (IP_PMTUDISC_DONT), %zu)", this->fd, this->fd, &flag, sizeof(flag));
		if (setsockopt(this->fd, IPPROTO_IP, IP_MTU_DISCOVER, &flag, sizeof(flag)) < 0) {
			ERROR("Failed disabling PMTU discovery: %s", fr_syserror(errno));

			close(this->fd);
			return -1;
		}
#endif

#if defined(IP_DONTFRAG)
		/*
		 *	Ensure that the "don't fragment" flag is zero.
		 */
		flag = 0;

		DEBUG4("[FD %i] Allowing IP fragmentation -- setsockopt(%i, IPPROTO_IP, IP_DONTFRAG, "
		       "%p (%u), %zu)", this->fd, this->fd, &flag, flag, sizeof(flag));
		if (setsockopt(this->fd, IPPROTO_IP, IP_DONTFRAG, &flag, sizeof(flag)) < 0) {
			ERROR("Failed setting don't fragment flag: %s", fr_syserror(errno));

			close(this->fd);
			return -1;
		}
#endif
	}

#if defined(WITH_DHCP) && defined(SO_BROADCAST)
	if (sock->broadcast) {
		int on = 1;

		DEBUG4("[FD %i] Enabling SO_BROADCAST -- setsockopt(%i, IPPROTO_IP, SO_BROADCAST, "
		       "%p (%u), %zu)", this->fd, this->fd, &on, on, sizeof(on));
		if (setsockopt(this->fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
			ERROR("Can't set broadcast option: %s", fr_syserror(errno));
			return -1;
		}
	}
#endif

	/*
	 *	May be binding to priviledged ports.
	 */
	if (sock->my_port != 0) {
		if (DEBUG_ENABLED4) {
			if (salocal.ss_family == AF_INET) {
				char		   	buffer[INET_ADDRSTRLEN];
				struct sockaddr_in	*addr = (struct sockaddr_in *)&salocal;

				inet_ntop(addr->sin_family, &addr->sin_addr, buffer, sizeof(buffer));
				DEBUG4("[FD %i] Binding to address %s port %u -- bind(%i, %p, %u)",
				       this->fd, buffer, ntohs(addr->sin_port), this->fd, &salocal, salen);
			} else if (salocal.ss_family == AF_INET6) {
				char		   	buffer[INET6_ADDRSTRLEN];
				struct sockaddr_in6	*addr = (struct sockaddr_in6 *)&salocal;

				inet_ntop(addr->sin6_family, &addr->sin6_addr, buffer, sizeof(buffer));
				DEBUG4("[FD %i] Binding to address %s port %u -- bind(%i, %p, %u)",
				       this->fd, buffer, ntohs(addr->sin6_port), this->fd, &salocal, salen);
			}
		}
		rad_suid_up();
		rcode = bind(this->fd, (struct sockaddr *)&salocal, salen);
		rad_suid_down();
		if (rcode < 0) {
			char buffer[256];

			close(this->fd);

			this->print(this, buffer, sizeof(buffer));
			ERROR("Failed binding to %s: %s", buffer, fr_syserror(errno));
			return -1;
		}

		/*
		 *	FreeBSD jail issues.  We bind to 0.0.0.0, but the
		 *	kernel instead binds us to a 1.2.3.4.  If this
		 *	happens, notice, and remember our real IP.
		 */
		{
			struct sockaddr_storage	src;
			socklen_t		sizeof_src = sizeof(src);

			memset(&src, 0, sizeof_src);
			if (getsockname(this->fd, (struct sockaddr *) &src, &sizeof_src) < 0) {
				ERROR("Failed getting socket name: %s", fr_syserror(errno));
				return -1;
			}

			if (!fr_ipaddr_from_sockaddr(&src, sizeof_src,
						&sock->my_ipaddr, &sock->my_port)) {
				ERROR("Socket has unsupported address family");
				return -1;
			}
		}
	}

#ifdef WITH_TCP
	if (sock->proto == IPPROTO_TCP) {
		/*
		 *	If there are hard-coded worker threads, OR
		 *	it's a TLS connection, it's blocking.
		 *
		 *	Otherwise, they're non-blocking.
		 */
		if (!this->workers
#  if defined(WITH_PROXY) && defined(WITH_TLS)
		    && (this->type == RAD_LISTEN_PROXY) && !this->tls
#  endif
			) {
			DEBUG4("[FD %i] Setting nonblock -- fr_nonblock(%i)", this->fd, this->fd);
			if (fr_nonblock(this->fd) < 0) {
				close(this->fd);
				ERROR("Failed setting non-blocking on socket: %s", fr_syserror(errno));
				return -1;
			}
		}

		/*
		 *	Allow a backlog of 8 listeners, but only for incoming interfaces.
		 */
#  ifdef WITH_PROXY
		if (this->type != RAD_LISTEN_PROXY)
#  endif
		DEBUG4("[FD %i] Listening -- listen(%i, 8)", this->fd, this->fd);
		if (listen(this->fd, 8) < 0) {
			close(this->fd);
			ERROR("Failed in listen(): %s", fr_syserror(errno));
			return -1;
		}
	}
#endif

	/*
	 *	Mostly for proxy sockets.
	 */
	sock->other_ipaddr.af = sock->my_ipaddr.af;

/*
 *	Don't screw up other people.
 */
#undef proto_for_port
#undef sock_type

	return 0;
}


static int _listener_free(rad_listen_t *this)
{
	/*
	 *	Other code may have eaten the FD.
	 */
	if (this->fd >= 0) close(this->fd);

	if (master_listen[this->type].free) {
		master_listen[this->type].free(this);
	}

#ifdef WITH_TCP
	if ((this->type == RAD_LISTEN_AUTH)
#ifdef WITH_ACCT
	    || (this->type == RAD_LISTEN_ACCT)
#endif
#ifdef WITH_PROXY
	    || (this->type == RAD_LISTEN_PROXY)
#endif
#ifdef WITH_COMMAND_SOCKET
	    || ((this->type == RAD_LISTEN_COMMAND) &&
		(((fr_command_socket_t *) this->data)->magic != COMMAND_SOCKET_MAGIC))
#endif
		) {
		listen_socket_t *sock = this->data;

		rad_assert(talloc_parent(sock) == this);
		rad_assert(sock->ev == NULL);

		/*
		 *	Remove the child from the parent tree.
		 */
		if (this->parent) {
			rbtree_deletebydata(this->parent->children, this);
		}

		/*
		 *	Delete / close all of the children, too!
		 */
		if (this->children) {
			rbtree_walk(this->children, RBTREE_DELETE_ORDER, listener_unlink, this);
		}

#ifdef WITH_TLS
		/*
		 *	Note that we do NOT free this->tls, as the
		 *	pointer is parented by its CONF_SECTION.  It
		 *	may be used by multiple listeners.
		 */
		if (this->tls) {
			rad_assert(!sock->ssn || (talloc_parent(sock->ssn) == sock));
			rad_assert(!sock->request || (talloc_parent(sock->request) == sock));
#ifdef HAVE_PTHREAD_H
			pthread_mutex_destroy(&(sock->mutex));
#endif
		}
#endif	/* WITH_TLS */
	}
#endif				/* WITH_TCP */

	return 0;
}


/*
 *	Allocate & initialize a new listener.
 */
static rad_listen_t *listen_alloc(TALLOC_CTX *ctx, RAD_LISTEN_TYPE type)
{
	rad_listen_t *this;

	this = talloc_zero(ctx, rad_listen_t);

	this->type = type;
	this->recv = master_listen[this->type].recv;
	this->send = master_listen[this->type].send;
	this->print = master_listen[this->type].print;
	this->debug = master_listen[this->type].debug;
	this->encode = master_listen[this->type].encode;
	this->decode = master_listen[this->type].decode;

	talloc_set_destructor(this, _listener_free);

	this->data = talloc_zero_array(this, uint8_t, master_listen[this->type].inst_size);

	return this;
}

#ifdef WITH_PROXY
/*
 *	Externally visible function for creating a new proxy LISTENER.
 *
 *	Not thread-safe, but all calls to it are protected by the
 *	proxy mutex in event.c
 */
rad_listen_t *proxy_new_listener(TALLOC_CTX *ctx, home_server_t *home, uint16_t src_port)
{
	time_t now;
	rad_listen_t *this;
	listen_socket_t *sock;
	char buffer[256];

	if (!home) return NULL;

	rad_assert(home->server == NULL); /* we only open real sockets */

	if ((home->limit.max_connections > 0) &&
	    (home->limit.num_connections >= home->limit.max_connections)) {
		RATE_LIMIT(INFO("Home server %s has too many open connections (%d)",
				home->log_name, home->limit.max_connections));
		return NULL;
	}

	now = time(NULL);
	if (home->last_failed_open == now) {
		WARN("Suppressing attempt to open socket to 'down' home server");
		return NULL;
	}

	this = listen_alloc(ctx, RAD_LISTEN_PROXY);

	sock = this->data;
	sock->other_ipaddr = home->ipaddr;
	sock->other_port = home->port;
	sock->home = home;

	sock->my_ipaddr = home->src_ipaddr;
	sock->my_port = src_port;
	sock->proto = home->proto;

	/*
	 *	For error messages.
	 */
	this->print(this, buffer, sizeof(buffer));

#ifdef WITH_TCP
	sock->opened = sock->last_packet = now;

	if (home->proto == IPPROTO_TCP) {
		this->recv = proxy_socket_tcp_recv;

		/*
		 *	FIXME: connect() is blocking!
		 *	We do this with the proxy mutex locked, which may
		 *	cause large delays!
		 *
		 *	http://www.developerweb.net/forum/showthread.php?p=13486
		 */
		this->fd = fr_socket_client_tcp(&home->src_ipaddr,
						&home->ipaddr, home->port, false);
	} else
#endif

		this->fd = fr_socket(&home->src_ipaddr, src_port);

	if (this->fd < 0) {
		this->print(this, buffer,sizeof(buffer));
		ERROR("Failed opening new proxy socket '%s' : %s",
		      buffer, fr_strerror());
		home->last_failed_open = now;
		listen_free(&this);
		return NULL;
	}


#if defined(WITH_TCP) && defined(WITH_TLS)
	if ((home->proto == IPPROTO_TCP) && home->tls) {
		DEBUG("Trying SSL to port %d\n", home->port);
		sock->ssn = tls_session_init_client(sock, home->tls, this->fd);
		if (!sock->ssn) {
			ERROR("Failed starting SSL to new proxy socket '%s'", buffer);
			home->last_failed_open = now;
			listen_free(&this);
			return NULL;
		}

		this->recv = proxy_tls_recv;
		this->send = proxy_tls_send;
	}
#endif
	/*
	 *	Figure out which port we were bound to.
	 */
	if (sock->my_port == 0) {
		struct sockaddr_storage	src;
		socklen_t		sizeof_src = sizeof(src);

		memset(&src, 0, sizeof_src);
		if (getsockname(this->fd, (struct sockaddr *) &src, &sizeof_src) < 0) {
			ERROR("Failed getting socket name for '%s': %s",
			      buffer, fr_syserror(errno));
			home->last_failed_open = now;
			listen_free(&this);
			return NULL;
		}

		if (!fr_ipaddr_from_sockaddr(&src, sizeof_src,
					&sock->my_ipaddr, &sock->my_port)) {
			ERROR("Socket has unsupported address family for '%s'", buffer);
			home->last_failed_open = now;
			listen_free(&this);
			return NULL;
		}

		this->print(this, buffer, sizeof(buffer));
	}

	if (rad_debug_lvl >= 3) {
		DEBUG("Opened new proxy socket '%s'", buffer);
	}

	home->limit.num_connections++;

	return this;
}
#endif

static int _free_proto_handle(lt_dlhandle *handle)
{
	dlclose(*handle);
	return 0;
}

static rad_listen_t *listen_parse(CONF_SECTION *cs, char const *server)
{
	int		type, rcode;
	char const	*listen_type;
	rad_listen_t	*this;
	CONF_PAIR	*cp;
	char const	*value;
	lt_dlhandle	handle;
	DICT_VALUE	*dv;
	CONF_SECTION	*server_cs;
	char		buffer[32];

	cp = cf_pair_find(cs, "type");
	if (!cp) {
		cf_log_err_cs(cs,
			   "No type specified in listen section");
		return NULL;
	}

	value = cf_pair_value(cp);
	if (!value) {
		cf_log_err_cp(cp,
			      "Type cannot be empty");
		return NULL;
	}

	snprintf(buffer, sizeof(buffer), "proto_%s", value);
	handle = lt_dlopenext(buffer);
	if (handle) {
		fr_protocol_t	*proto;
		lt_dlhandle	*marker;

		proto = dlsym(handle, buffer);
		if (!proto) {
			cf_log_err_cs(cs,
				      "Failed linking to protocol %s : %s\n",
				      value, dlerror());
			dlclose(handle);
			return NULL;
		}

		/*
		 *	We need numbers for internal use.
		 */
		dv = dict_valbyname(1147, 0, value);
		if (!dv) {
			if (dict_addvalue(value, "Listen-Socket-Type",
				    last_listener) < 0) {
				cf_log_err_cs(cs,
					      "Failed adding dictionary entry for protocol %s: %s",
					      value, fr_strerror());
				dlclose(handle);
				return NULL;
			}

			dv = dict_valbyname(1147, 0, value);
			if (!dv) {
				cf_log_err_cs(cs, "Failed finding dictionary entry for protocol %s",
					      value);
				dlclose(handle);
				return NULL;
			}
			last_listener++;
			if (last_listener >= MAX_LISTENER) {
				cf_log_err_cs(cs, "Too many listeners at protocol %s",
					      value);
				dlclose(handle);
				return NULL;
			}
		}

		type = dv->value;

		/*
		 *	FIXME: malloc this, or put it into a tree.
		 */
		memcpy(&master_listen[type], proto, sizeof(*proto));

		/*
		 *	Ensure handle gets closed if config section gets freed
		 */
		marker = talloc(cs, lt_dlhandle);
		*marker = handle;
		talloc_set_destructor(marker, _free_proto_handle);

		if (master_listen[type].magic !=  RLM_MODULE_INIT) {
			ERROR("Failed to load protocol '%s', it has the wrong version.",
			       master_listen[type].name);
			return NULL;
		}
	}

	cf_log_info(cs, "listen {");

	listen_type = NULL;
	rcode = cf_item_parse(cs, "type", FR_ITEM_POINTER(PW_TYPE_STRING, &listen_type), "", T_DOUBLE_QUOTED_STRING);
	if (rcode < 0) return NULL;
	if (rcode == 1) {
		cf_log_err_cs(cs,
			   "No type specified in listen section");
		return NULL;
	}

	/*
	 *	Couldn't link to it.  It MUST be defined in the
	 *	dictionaries.
	 */
	dv = dict_valbyname(1147, 0, value);
	if (!dv) {
		cf_log_err_cs(cs, "No dictionary entry for protocol %s",
			      value);
		return NULL;
	}

	if ((dv->value >= MAX_LISTENER) ||
	    (master_listen[dv->value].magic == 0)) {
		cf_log_err_cs(cs, "Failed finding plugin for protocol %s",
			      value);
		return NULL;
	}

	type = dv->value;

	if ((strcmp(master_listen[type].name, dv->name) != 0) &&
	    !strchr(dv->name, '+')) {
		cf_log_err_cs(cs, "Inconsistent dictionaries for protocol %s",
			      value);
		return NULL;
	}

	/*
	 *	DHCP and VMPS *must* be loaded dynamically.
	 */
	if (master_listen[type].magic !=  RLM_MODULE_INIT) {
		ERROR("Cannot load protocol '%s', as the required library does not exist",
		      master_listen[type].name);
		return NULL;
	}

	/*
	 *	Allow listen sections in the default config to
	 *	refer to a server.
	 */
	if (!server) {
		rcode = cf_item_parse(cs, "virtual_server",
				      FR_ITEM_POINTER(PW_TYPE_STRING, &server), NULL, T_DOUBLE_QUOTED_STRING);
		if (rcode < 0) return NULL;
	}

#ifdef WITH_PROXY
	/*
	 *	We were passed a virtual server, so the caller is
	 *	defining a proxy listener inside of a virtual server.
	 *	This isn't allowed right now.
	 */
	else if (type == RAD_LISTEN_PROXY) {
		ERROR("Error: listen type \"proxy\" Cannot appear in a virtual server section");
		return NULL;
	}
#endif

	/*
	 *	Set up cross-type data.
	 */
	this = listen_alloc(cs, type);
	this->server = server;
	this->fd = -1;

#ifdef WITH_TCP
	/*
	 *	Special-case '+' for "auth+acct".
	 */
	if (strchr(listen_type, '+') != NULL) {
		this->dual = true;
	}
#endif

	/*
	 *	Call per-type parser.
	 */
	if (master_listen[type].parse(cs, this) < 0) {
		listen_free(&this);
		return NULL;
	}

	server_cs = cf_section_sub_find_name2(main_config.config, "server",
					      this->server);
	if (!server_cs && this->server) {
		cf_log_err_cs(cs, "No such server \"%s\"", this->server);
		listen_free(&this);
		return NULL;
	}

	cf_log_info(cs, "}");

	return this;
}


#ifdef HAVE_PTHREAD_H
/*
 *	A child thread which does NOTHING other than read and process
 *	packets.
 */
static void *recv_thread(void *arg)
{
	rad_listen_t *this = arg;

	while (1) {
		this->recv(this);
		DEBUG("%p", &this);
	}

	return NULL;
}
#endif


/** Search for listeners in the server
 *
 * @param[in] config to search for listener sections in.
 * @param[out] head Where to write listener.  Must point to a NULL pointer.
 * @param[in] spawn_workers Whether we're spawning child threads.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int listen_init(CONF_SECTION *config, rad_listen_t **head, bool spawn_workers)
{
	CONF_SECTION	*cs = NULL;
	rad_listen_t	**last;
	rad_listen_t	*this;

	/*
	 *	We shouldn't be called with a pre-existing list.
	 */
	rad_assert(head && (*head == NULL));

	last = head;

	/*
         *      Walk through the "listen" sections, if they exist.
         */
	for (cs = cf_subsection_find_next(config, NULL, "listen");
	     cs != NULL;
	     cs = cf_subsection_find_next(config, cs, "listen")) {
		this = listen_parse(cs, NULL);
		if (!this) {
			listen_free(head);
			return -1;
		}
		if (this->type != RAD_LISTEN_COMMAND) {
			cf_log_err_cs(this->cs, "Only listen sections of type = command (command sockets)"
				      "are allowed outside of server sections");
			listen_free(head);
			return -1;
		}

		*last = this;
		last = &(this->next);
	}

	/*
	 *	Check virtual servers for "listen" sections
	 *
	 *	FIXME: Move to virtual server init?
	 */
	for (cs = cf_subsection_find_next(config, NULL, "server");
	     cs != NULL;
	     cs = cf_subsection_find_next(config, cs, "server")) {
		CONF_SECTION *subcs;
		char const *name2 = cf_section_name2(cs);

 		/*
 		 *	Loop over "listen" directives in virtual servers
 		 */
		for (subcs = cf_subsection_find_next(cs, NULL, "listen");
		     subcs != NULL;
		     subcs = cf_subsection_find_next(cs, subcs, "listen")) {
			this = listen_parse(subcs, name2);
			if (!this) {
				listen_free(head);
				return -1;
			}

			*last = this;
			last = &(this->next);
		}
	}

	/*
	 *	No sockets to receive packets, this is an error
	 *	proxying is pointless.
	 */
	if (!*head) {
		ERROR("The server is not configured to listen on any ports.  Cannot start");
		return -1;
	}

	/*
	 *	Print out which sockets we're listening on, and
	 *	add them to the event list.
	 */
	for (this = *head; this != NULL; this = this->next) {
#ifdef WITH_TLS
		if (!check_config && !spawn_workers && this->tls) {
			cf_log_err_cs(this->cs, "Threading must be enabled for TLS sockets to function properly");
			cf_log_err_cs(this->cs, "You probably need to do '%s -fxx -l stdout' for debugging",
				      progname);
			return -1;
		}
#endif
		if (!check_config) {
			if (this->workers && !spawn_workers) {
				WARN("Setting 'workers' requires 'synchronous'.  Disabling 'workers'");
				this->workers = 0;
			}

			if (this->workers) {
#ifdef HAVE_PTHREAD_H
				int rcode;
				uint32_t i;
				char buffer[256];

				this->print(this, buffer, sizeof(buffer));

				for (i = 0; i < this->workers; i++) {
					pthread_t id;

					/*
					 *	FIXME: create detached?
					 */
					rcode = pthread_create(&id, 0, recv_thread, this);
					if (rcode != 0) {
						ERROR("Thread create failed: %s", fr_syserror(rcode));
						fr_exit(1);
					}

					DEBUG("Thread %d for %s\n", i, buffer);
				}
#else
				WARN("Setting 'workers' requires 'synchronous'.  Disabling 'workers'");
				this->workers = 0;
#endif

			} else {
				radius_update_listener(this);
			}

		}
	}

	/*
	 *	Haven't defined any sockets.  Die.
	 */
	if (!*head) return -1;

	return 0;
}

/** Free a linked list of listeners
 *
 * @param head of list to free.
 */
void listen_free(rad_listen_t **head)
{
	rad_listen_t *this;

	if (!head || !*head) return;

	this = *head;
	while (this) {
		rad_listen_t *next = this->next;
		talloc_free(this);
		this = next;
	}

	*head = NULL;
}

#ifdef WITH_STATS
/** Find client list associated with a listener.
 *
 * @param[in] ipaddr listener is bound to.
 * @param[in] port listener is bound to.
 * @param[in] proto of listener, one of the IPPROTO_* macros.
 * @return
 *	- List of clients.
 *	- NULL if no matching listeners found.
 */
RADCLIENT_LIST *listener_find_client_list(fr_ipaddr_t const *ipaddr, uint16_t port, int proto)
{
	rad_listen_t *this;

	for (this = main_config.listen; this != NULL; this = this->next) {
		listen_socket_t *sock;

		if ((this->type != RAD_LISTEN_AUTH)
#  ifdef WITH_ACCOUNTING
		    && (this->type != RAD_LISTEN_ACCT)
#  endif
#  ifdef WITH_COA
		    && (this->type != RAD_LISTEN_COA)
#  endif
		    ) continue;

		sock = this->data;

		if (sock->my_port != port) continue;
		if (sock->proto != proto) continue;
		if (fr_ipaddr_cmp(ipaddr, &sock->my_ipaddr) != 0) continue;

		return sock->clients;
	}

	return NULL;
}
#endif

/** Find a listener associated with an IP address/port/transport proto
 *
 * @param[in] ipaddr listener is bound to.
 * @param[in] port listener is bound to.
 * @param[in] proto of listener, one of the IPPROTO_* macros.
 * @return
 *	- Listener matching ipaddr/port/proto.
 *	- NULL if no listeners match.
 */
rad_listen_t *listener_find_byipaddr(fr_ipaddr_t const *ipaddr, uint16_t port, int proto)
{
	rad_listen_t *this;

	for (this = main_config.listen; this != NULL; this = this->next) {
		listen_socket_t *sock;

		sock = this->data;

		if (sock->my_port != port) continue;
		if (sock->proto != proto) continue;
		if (fr_ipaddr_cmp(ipaddr, &sock->my_ipaddr) != 0) continue;

		return this;
	}

	/*
	 *	Failed to find a specific one.  Find INADDR_ANY
	 */
	for (this = main_config.listen; this != NULL; this = this->next) {
		listen_socket_t *sock;

		sock = this->data;

		if (sock->my_port != port) continue;
		if (sock->proto != proto) continue;
		if (!fr_is_inaddr_any(&sock->my_ipaddr)) continue;

		return this;
	}

	return NULL;
}
