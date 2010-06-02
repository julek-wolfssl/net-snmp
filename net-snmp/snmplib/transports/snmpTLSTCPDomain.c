#include <net-snmp/net-snmp-config.h>

#include <net-snmp/library/snmpTLSTCPDomain.h>

#include <stdio.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>

#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#if HAVE_NETDB_H
#include <netdb.h>
#endif
#if HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#include <net-snmp/types.h>
#include <net-snmp/output_api.h>
#include <net-snmp/config_api.h>
#include <net-snmp/library/snmp_assert.h>
#include <net-snmp/library/snmpIPv4BaseDomain.h>
#include <net-snmp/library/snmpSocketBaseDomain.h>
#include <net-snmp/library/snmpTLSBaseDomain.h>
#include <net-snmp/library/system.h>
#include <net-snmp/library/tools.h>
#include <net-snmp/library/cert_util.h>
#include <net-snmp/library/snmp_openssl.h>
#include <net-snmp/library/callback.h>

#include "openssl/bio.h"
#include "openssl/ssl.h"
#include "openssl/err.h"

#ifndef INADDR_NONE
#define INADDR_NONE	-1
#endif

#define WE_ARE_SERVER 0
#define WE_ARE_CLIENT 1

oid             netsnmpTLSTCPDomain[] = { TRANSPORT_DOMAIN_TLS_TCP_IP };
size_t          netsnmpTLSTCPDomain_len = OID_LENGTH(netsnmpTLSTCPDomain);

static netsnmp_tdomain tlstcpDomain;

/*
 * Return a string representing the address in data, or else the "far end"
 * address if data is NULL.  
 */

static char *
netsnmp_tlstcp_fmtaddr(netsnmp_transport *t, void *data, int len)
{
    return netsnmp_ipv4_fmtaddr("TLSTCP", t, data, len);
}
/*
 * You can write something into opaque that will subsequently get passed back 
 * to your send function if you like.  For instance, you might want to
 * remember where a PDU came from, so that you can send a reply there...  
 */

static int
netsnmp_tlstcp_copy(netsnmp_transport *oldt, netsnmp_transport *newt)
{
    _netsnmpTLSBaseData *oldtlsdata = (_netsnmpTLSBaseData *) oldt->data;
    _netsnmpTLSBaseData *newtlsdata = (_netsnmpTLSBaseData *) newt->data;
    oldtlsdata->accepted_bio = NULL;
    oldtlsdata->ssl = NULL;
    newtlsdata->ssl_context = NULL;
    return 0;
}

static int
netsnmp_tlstcp_recv(netsnmp_transport *t, void *buf, int size,
                    void **opaque, int *olength)
{
    int             rc = -1;
    netsnmp_indexed_addr_pair *addr_pair = NULL;
    struct sockaddr *from;
    netsnmp_tmStateReference *tmStateRef = NULL;
    _netsnmpTLSBaseData *tlsdata;

    if (NULL == t || t->sock < 0 || NULL == t->data) {
        snmp_log(LOG_ERR,
                 "tlstcp received an invalid invocation with missing data\n");
        DEBUGMSGTL(("tlstcp", "recvfrom fd %d err %d (\"%s\")\n",
                    t->sock, errno, strerror(errno)));
        DEBUGMSGTL(("tlstcp", "  tdata = %x\n", (uintptr_t)t->data));
        return -1;
    }
        
    /* RFCXXXX Section 5.1.2 step 1:
    1) Determine the tlstmSessionID for the incoming message. The
       tlstmSessionID MUST be a unique session identifier for this
       (D)TLS connection.  The contents and format of this identifier
       are implementation-dependent as long as it is unique to the
       session.  A session identifier MUST NOT be reused until all
       references to it are no longer in use.  The tmSessionID is
       equal to the tlstmSessionID discussed in Section 5.1.1.
       tmSessionID refers to the session identifier when stored in the
       tmStateReference and tlstmSessionID refers to the session
       identifier when stored in the LCD.  They MUST always be equal
       when processing a given session's traffic.
     */
    /* For this implementation we use the t->data memory pointer as
       the sessionID.  As it's a pointer to session specific data tied
       with the transport object we know it'll never be realloated
       (ie, duplicated) until release by this transport object and is
       safe to use as a unique session identifier. */

    tlsdata = t->data;
    if (NULL == tlsdata->ssl) {
        snmp_log(LOG_ERR,
                 "tlstcp received an invalid invocation without ssl data\n");
        return -1;
    }

    /* RFCXXXX Section 5.1.2 step 1, part2:
     * This part (incrementing the counter) is done in the
       netsnmp_tlstcp_accept function.
     */


    /* RFCXXXX Section 5.1.2 step 2:
     * Create a tmStateReference cache for the subsequent reference and
       assign the following values within it:

       tmTransportDomain  = snmpTLSTCPDomain or snmpDTLSUDPDomain as
                            appropriate.

       tmTransportAddress = The address the message originated from.

       tmSecurityLevel    = The derived tmSecurityLevel for the session,
                            as discussed in Section 3.1.2 and Section 5.3.

       tmSecurityName     = The fderived tmSecurityName for the session as
                            discussed in Section 5.3.  This value MUST
                            be constant during the lifetime of the
                            session.

       tmSessionID        = The tlstmSessionID described in step 1 above.
     */

    /* Implementation notes:
     * - The tmTransportDomain is represented by the transport object
     * - The tmpSessionID is represented by the tlsdata pointer (as
         discussed above)
     * - The following items are handled later in netsnmp_tlsbase_wrapup_recv:
         - tmSecurityLevel
         - tmSecurityName
         - tmSessionID
    */

    /* create a tmStateRef cache for slow fill-in */
    tmStateRef = SNMP_MALLOC_TYPEDEF(netsnmp_tmStateReference);

    if (tmStateRef == NULL) {
        *opaque = NULL;
        *olength = 0;
        return -1;
    }

    /* Set the transportDomain */
    memcpy(tmStateRef->transportDomain,
           netsnmpTLSTCPDomain, sizeof(netsnmpTLSTCPDomain[0]) *
           netsnmpTLSTCPDomain_len);
    tmStateRef->transportDomainLen = netsnmpTLSTCPDomain_len;

    /* Set the tmTransportAddress */
    addr_pair = &tmStateRef->addresses;
    tmStateRef->have_addresses = 1;
    from = (struct sockaddr *) &(addr_pair->remote_addr);

    /* RFCXXXX Section 5.1.2 step 1:
     * 3)  The incomingMessage and incomingMessageLength are assigned values
       from the (D)TLS processing.
     */

    /* Implementation notes:
       - incomingMessage       = buf pointer
       - incomingMessageLength = rc
    */

    /* read the packet from openssl */
    rc = SSL_read(tlsdata->ssl, buf, size);
    while (rc <= 0) {
        if (rc == 0) {
            /* XXX closed connection */
            DEBUGMSGTL(("tlstcp", "remote side closed connection\n"));
            /* XXX: openssl cleanup */
            SNMP_FREE(tmStateRef);
            return -1;
        }
        rc = SSL_read(tlsdata->ssl, buf, size);
    }

    DEBUGMSGTL(("tlstcp", "received %d decoded bytes from tls\n", rc));

    /* Check for errors */
    if (rc == -1) {
        if (SSL_get_error(tlsdata->ssl, rc) == SSL_ERROR_WANT_READ)
            return -1; /* XXX: it's ok, but what's the right return? */

        _openssl_log_error(rc, tlsdata->ssl, "SSL_read");
        SNMP_FREE(tmStateRef);

        return rc;
    }

    /* log the packet */
    {
        char *str = netsnmp_tlstcp_fmtaddr(NULL, addr_pair, sizeof(netsnmp_indexed_addr_pair));
        DEBUGMSGTL(("tlstcp",
                    "recvfrom fd %d got %d bytes (from %s)\n",
                    t->sock, rc, str));
        free(str);
    }

    /* Other wrap-up things common to TLS and DTLS */
    if (netsnmp_tlsbase_wrapup_recv(tmStateRef, tlsdata, opaque, olength) !=
        SNMPERR_SUCCESS)
        return SNMPERR_GENERR;

    /* RFCXXXX Section 5.1.2 step 1:
     * 4)  The TLS Transport Model passes the transportDomain,
       transportAddress, incomingMessage, and incomingMessageLength to
       the Dispatcher using the receiveMessage ASI:
    */

    /* In our implementation, this is done simply by returning */
    return rc;
}



static int
netsnmp_tlstcp_send(netsnmp_transport *t, void *buf, int size,
		 void **opaque, int *olength)
{
    int rc = -1;
    netsnmp_tmStateReference *tmStateRef = NULL;
    _netsnmpTLSBaseData *tlsdata;
    
    DEBUGTRACETOK("tlstcp");

    /* RFCXXXX section 5.2: 
      1)  If tmStateReference does not refer to a cache containing values
      for tmTransportDomain, tmTransportAddress, tmSecurityName,
      tmRequestedSecurityLevel, and tmSameSecurity, then increment the
      snmpTlstmSessionInvalidCaches counter, discard the message, and
      return the error indication in the statusInformation.  Processing
      of this message stops.
    */
    /* Implementation Notes: the tmStateReference is stored in the opaque ptr */
    if (opaque != NULL && *opaque != NULL &&
        *olength == sizeof(netsnmp_tmStateReference)) {
        tmStateRef = (netsnmp_tmStateReference *) *opaque;
    } else {
        snmp_log(LOG_ERR, "TLSTCP was called with an invalid state; possibly the wrong security model is in use.  It should be 'tsm'.\n");
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONINVALIDCACHES);
        return SNMPERR_GENERR;
    }


    /* RFCXXXX section 5.2: 
       2)  Extract the tmSessionID, tmTransportDomain, tmTransportAddress,
       tmSecurityName, tmRequestedSecurityLevel, and tmSameSecurity
       values from the tmStateReference.  Note: The tmSessionID value
       may be undefined if no session exists yet over which the message
       can be sent.
    */
    /* Implementation Notes:
       - Our session will always exist by now as it's created when the
         transport object is created. Auto-session creation is handled
         higher in the stack.
       - We don't "extract" per say since we just leave the data in
         the structure.
       - The sessionID is stored in the t->data memory pointer.
    */

    /* RFCXXXX section 5.2: 
       3)  If tmSameSecurity is true and either tmSessionID is undefined or
           refers to a session that is no longer open then increment the
           snmpTlstmSessionNoSessions counter, discard the message and
           return the error indication in the statusInformation.  Processing
           of this message stops.
    */
    /* Implementation Notes:
       - We would never get here if the sessionID was either undefined
         or different.  We tie packets directly to the transport
         object and it could never be sent back over a different
         transport, which is what the above text is trying to prevent.
     */

    /* RFCXXXX section 5.2: 
       4)  If tmSameSecurity is false and tmSessionID refers to a session
           that is no longer available then an implementation SHOULD open a
           new session using the openSession() ASI (described in greater
           detail in step 5b).  Instead of opening a new session an
           implementation MAY return a snmpTlstmSessionNoSessions error to
           the calling module and stop processing of the message.
    */
    /* Implementation Notes:
       - We would never get here if the sessionID was either undefined
         or different.  We tie packets directly to the transport
         object and it could never be sent back over a different
         transport, which is what the above text is trying to prevent.
       - Auto-connections are handled higher in the Net-SNMP library stack
     */
    
    /* RFCXXXX section 5.2: 
       5)  If tmSessionID is undefined, then use tmTransportDomain,
           tmTransportAddress, tmSecurityName and tmRequestedSecurityLevel
           to see if there is a corresponding entry in the LCD suitable to
           send the message over.

           5a)  If there is a corresponding LCD entry, then this session
                will be used to send the message.

           5b)  If there is not a corresponding LCD entry, then open a
                session using the openSession() ASI (discussed further in
                Section 5.3.1).  Implementations MAY wish to offer message
                buffering to prevent redundant openSession() calls for the
                same cache entry.  If an error is returned from
                openSession(), then discard the message, discard the
                tmStateReference, increment the snmpTlstmSessionOpenErrors,
                return an error indication to the calling module and stop
                processing of the message.
    */
    /* Implementation Notes:
       - We would never get here if the sessionID was either undefined
         or different.  We tie packets directly to the transport
         object and it could never be sent back over a different
         transport, which is what the above text is trying to prevent.
       - Auto-connections are handled higher in the Net-SNMP library stack
     */

    /* our session pointer is functionally t->data */
    if (NULL == t->data) {
        snmp_log(LOG_ERR, "netsnmp_tlstcp_send received no incoming data\n");
        return -1;
    }

    tlsdata = t->data;
    
    /* If the first packet and we have no secname, then copy the
       important securityName data into the longer-lived session
       reference information. */
    if ((tlsdata->flags | NETSNMP_TLSBASE_IS_CLIENT) &&
        !tlsdata->securityName && tmStateRef && tmStateRef->securityNameLen > 0)
        tlsdata->securityName = strdup(tmStateRef->securityName);
        
        
    /* RFCXXXX section 5.2: 
       6)  Using either the session indicated by the tmSessionID if there
           was one or the session resulting from a previous step (4 or 5),
           pass the outgoingMessage to (D)TLS for encapsulation and
           transmission.
    */
    rc = SSL_write(tlsdata->ssl, buf, size);
    DEBUGMSGTL(("tlstcp", "wrote %d bytes\n", size));
    if (rc < 0) {
        _openssl_log_error(rc, tlsdata->ssl, "SSL_write");
    }

    return rc;
}



static int
netsnmp_tlstcp_close(netsnmp_transport *t)
{
    _netsnmpTLSBaseData *tlsdata;

    if (NULL == t || NULL == t->data)
        return -1;

    /* RFCXXXX Section 5.4.  Closing a Session

       1)  Increment either the snmpTlstmSessionClientCloses or the
           snmpTlstmSessionServerCloses counter as appropriate.
    */
    if (t->flags & NETSNMP_TLSBASE_IS_CLIENT)
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONCLIENTCLOSES);
    else 
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONSERVERCLOSES);

    /* RFCXXXX Section 5.4.  Closing a Session
       2)  Look up the session using the tmSessionID.
    */
    tlsdata = (_netsnmpTLSBaseData *) t->data;

    /* RFCXXXX Section 5.4.  Closing a Session
       3)  If there is no open session associated with the tmSessionID, then
           closeSession processing is completed.
    */
    /* Implementation notes: if we have a non-zero tlsdata then it's
       always true */

    /* RFCXXXX Section 5.3.1: Establishing a Session as a Client
       4)  Have (D)TLS close the specified connection.  This SHOULD include
           sending a close_notify TLS Alert to inform the other side that
           session cleanup may be performed.
    */
    if (tlsdata->ssl) {
        SSL_shutdown(tlsdata->ssl);
        tlsdata->ssl = NULL;
    }

    if (tlsdata->sslbio) {
        BIO_free(tlsdata->sslbio);
        tlsdata->sslbio = NULL;
    }

    if (tlsdata->accepted_bio) {
        BIO_free(tlsdata->accepted_bio);
        tlsdata->accepted_bio = NULL;
    }

    SNMP_FREE(tlsdata->securityName);
    SNMP_FREE(tlsdata->our_identity);
    SNMP_FREE(tlsdata->their_identity);

    /* don't free the accept_bio since it's the parent bio */
    SNMP_FREE(tlsdata);
    t->data = NULL;
    return netsnmp_socketbase_close(t);
}

static int
netsnmp_tlstcp_accept(netsnmp_transport *t)
{
    char           *str = NULL;
    BIO            *accepted_bio;
    int             rc;
    SSL_CTX *ctx;
    SSL     *ssl;
    _netsnmpTLSBaseData *tlsdata = NULL;
    
    DEBUGMSGTL(("tlstcp", "netsnmp_tlstcp_accept called\n"));

    tlsdata = (_netsnmpTLSBaseData *) t->data;

    rc = BIO_do_accept(tlsdata->accept_bio);

    if (rc <= 0) {
        snmp_log(LOG_ERR, "BIO_do_accept failed\n");
        _openssl_log_error(rc, NULL, "BIO_do_accept");
        /* XXX: need to close the listening connection here? */
        return -1;
    }

    tlsdata->accepted_bio = accepted_bio = BIO_pop(tlsdata->accept_bio);
    if (!accepted_bio) {
        snmp_log(LOG_ERR, "Failed to pop an accepted bio off the bio staack\n");
        /* XXX: need to close the listening connection here? */
        return -1;
    }

    /* create the OpenSSL TLS context */
    ctx = tlsdata->ssl_context;

    /* create the server's main SSL bio */
    ssl = tlsdata->ssl = SSL_new(ctx);
    if (!tlsdata->ssl) {
        snmp_log(LOG_ERR, "TLSTCP: Failed to create a SSL BIO\n");
        return -1;
    }
        
    SSL_set_bio(ssl, accepted_bio, accepted_bio);
        
    if ((rc = SSL_accept(ssl)) <= 0) {
        snmp_log(LOG_ERR, "TLSTCP: Failed SSL_accept\n");
        return -1;
    }   

    /* RFCXXXX Section 5.3.2: Accepting a Session as a Server
       A (D)TLS server should accept new session connections from any client
       that it is able to verify the client's credentials for.  This is done
       by authenticating the client's presented certificate through a
       certificate path validation process (e.g.  [RFC5280]) or through
       certificate fingerprint verification using fingerprints configured in
       the snmpTlstmCertToTSNTable.  Afterward the server will determine the
       identity of the remote entity using the following procedures.

       The (D)TLS server identifies the authenticated identity from the
       (D)TLS client's principal certificate using configuration information
       from the snmpTlstmCertToTSNTable mapping table.  The (D)TLS server
       MUST request and expect a certificate from the client and MUST NOT
       accept SNMP messages over the (D)TLS connection until the client has
       sent a certificate and it has been authenticated.  The resulting
       derived tmSecurityName is recorded in the tmStateReference cache as
       tmSecurityName.  The details of the lookup process are fully
       described in the DESCRIPTION clause of the snmpTlstmCertToTSNTable
       MIB object.  If any verification fails in any way (for example
       because of failures in cryptographic verification or because of the
       lack of an appropriate row in the snmpTlstmCertToTSNTable) then the
       session establishment MUST fail, and the
       snmpTlstmSessionInvalidClientCertificates object is incremented.  If
       the session can not be opened for any reason at all, including
       cryptographic verification failures, then the
       snmpTlstmSessionOpenErrors counter is incremented and processing
       stops.

       Servers that wish to support multiple principals at a particular port
       SHOULD make use of a (D)TLS extension that allows server-side
       principal selection like the Server Name Indication extension defined
       in Section 3.1 of [RFC4366].  Supporting this will allow, for
       example, sending notifications to a specific principal at a given TCP
       or UDP port.
    */
    /* Implementation notes:
       - we expect fingerprints to be stored in the transport config
       - we do not currently support mulitple principals and only offer one
    */
    if ((rc = netsnmp_tlsbase_verify_client_cert(ssl, tlsdata))
        != SNMPERR_SUCCESS) {
        /* XXX: free needed memory */
        snmp_log(LOG_ERR, "TLSTCP: Falied checking client certificate\n");
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONINVALIDCLIENTCERTIFICATES);
        return -1;
    }


    /* XXX: check acceptance criteria here */

    DEBUGMSGTL(("tlstcp", "accept succeeded (from %s) on sock %d\n",
                str, t->sock));
    free(str);

    /* RFCXXXX Section 5.1.2 step 1, part2::
     * If this is the first message received through this session and
     the session does not have an assigned tlstmSessionID yet then the
     snmpTlstmSessionAccepts counter is incremented and a
     tlstmSessionID for the session is created.  This will only happen
     on the server side of a connection because a client would have
     already assigned a tlstmSessionID during the openSession()
     invocation.  Implementations may have performed the procedures
     described in Section 5.3.2 prior to this point or they may
     perform them now, but the procedures described in Section 5.3.2
     MUST be performed before continuing beyond this point.
    */
    /* We're taking option 2 and incrementing the session accepts here
       rather than upon receiving the first packet */
    snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONACCEPTS);

    /* XXX: check that it returns something so we can free stuff? */
    return BIO_get_fd(tlsdata->accepted_bio, NULL);
}


netsnmp_transport *
netsnmp_tlstcp_open(netsnmp_transport *t)
{
    _netsnmpTLSBaseData *tlsdata;
    BIO *bio;
    SSL_CTX *ctx;
    SSL *ssl;
    char tmpbuf[128];
    int rc = 0;

    netsnmp_assert_or_return(t != NULL, NULL);
    netsnmp_assert_or_return(t->data != NULL, NULL);
    netsnmp_assert_or_return(sizeof(_netsnmpTLSBaseData) == t->data_length,
                             NULL);

    tlsdata = t->data;
    
    if (tlsdata->flags & NETSNMP_TLSBASE_IS_CLIENT) {
        /* Is the client */

        /* RFCXXXX Section 5.3.1:  Establishing a Session as a Client
         *    1)  The snmpTlstmSessionOpens counter is incremented.
         */
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONOPENS);

        /* RFCXXXX Section 5.3.1:  Establishing a Session as a Client
          2)  The client selects the appropriate certificate and cipher_suites
              for the key agreement based on the tmSecurityName and the
              tmRequestedSecurityLevel for the session.  For sessions being
              established as a result of a SNMP-TARGET-MIB based operation, the
              certificate will potentially have been identified via the
              snmpTlstmParamsTable mapping and the cipher_suites will have to
              be taken from system-wide or implementation-specific
              configuration.  If no row in the snmpTlstmParamsTable exists then
              implementations MAY choose to establish the connection using a
              default client certificate available to the application.
              Otherwise, the certificate and appropriate cipher_suites will
              need to be passed to the openSession() ASI as supplemental
              information or configured through an implementation-dependent
              mechanism.  It is also implementation-dependent and possibly
              policy-dependent how tmRequestedSecurityLevel will be used to
              influence the security capabilities provided by the (D)TLS
              connection.  However this is done, the security capabilities
              provided by (D)TLS MUST be at least as high as the level of
              security indicated by the tmRequestedSecurityLevel parameter.
              The actual security level of the session is reported in the
              tmStateReference cache as tmSecurityLevel.  For (D)TLS to provide
              strong authentication, each principal acting as a command
              generator SHOULD have its own certificate.
        */
        /*
          Implementation notes: we do most of this in the
          sslctx_client_setup The transport should have been
          f_config()ed with the proper fingerprints to use (which is
          stored in tlsdata), or we'll use the default identity
          fingerprint if that can be found.
        */

        /* XXX: check securityLevel and ensure no NULL fingerprints are used */

        /* set up the needed SSL context */
        tlsdata->ssl_context = ctx =
            sslctx_client_setup(TLSv1_method(), tlsdata);
        if (!ctx) {
            snmp_log(LOG_ERR, "failed to create TLS context\n");
            return NULL;
        }

        /* RFCXXXX Section 5.3.1:  Establishing a Session as a Client
           3)  Using the destTransportDomain and destTransportAddress values,
               the client will initiate the (D)TLS handshake protocol to
               establish session keys for message integrity and encryption.
        */
        /* Implementation note:
           The transport domain and address are pre-processed by this point
        */

        /* create the openssl connection string host:port */
        snprintf(tmpbuf, sizeof(tmpbuf), "%s:%d",
                 inet_ntoa(tlsdata->addr.sin_addr),
                 ntohs(tlsdata->addr.sin_port));

        /* Create a BIO connection for it */
        DEBUGMSGTL(("tlstcp", "connecting to tlstcp %s\n", tmpbuf));
        bio = BIO_new_connect(tmpbuf);

        /* RFCXXXX Section 5.3.1:  Establishing a Session as a Client
           3) continued:
              If the attempt to establish a session is unsuccessful, then
              snmpTlstmSessionOpenErrors is incremented, an error indication is
              returned, and processing stops.
        */

        if (NULL == bio) {
            snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONOPENERRORS);
            snmp_log(LOG_ERR, "tlstcp: failed to create bio\n");
            _openssl_log_error(rc, NULL, "BIO creation");
            SNMP_FREE(tlsdata);
            SNMP_FREE(t);
            return NULL;
        }
            

        /* Tell the BIO to actually do the connection */
        if ((rc = BIO_do_connect(bio)) <= 0) {
            snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONOPENERRORS);
            snmp_log(LOG_ERR, "tlstcp: failed to connect to %s\n", tmpbuf);
            _openssl_log_error(rc, NULL, "BIO_do_connect");
            BIO_free(bio);
            SNMP_FREE(tlsdata);
            SNMP_FREE(t);
            return NULL;
        }

        /* Create the SSL layer on top of the socket bio */
        ssl = tlsdata->ssl = SSL_new(ctx);
        if (NULL == ssl) {
            snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONOPENERRORS);
            snmp_log(LOG_ERR, "tlstcp: failed to create a SSL connection\n");
            BIO_free(bio);
            SNMP_FREE(tlsdata);
            SNMP_FREE(t);
            return NULL;
        }
        
        /* Bind the SSL layer to the BIO */ 
        SSL_set_bio(ssl, bio, bio);
        SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

        /* Then have SSL do it's connection over the BIO */
        if ((rc = SSL_connect(ssl)) <= 0) {
            snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONOPENERRORS);
            snmp_log(LOG_ERR, "tlstcp: failed to ssl_connect\n");
            BIO_free(bio);
            SNMP_FREE(tlsdata);
            SNMP_FREE(t);
            return NULL;
        }

        /* RFCXXXX Section 5.3.1: Establishing a Session as a Client
           3) continued:
              If the session failed to open because the presented
              server certificate was unknown or invalid then the
              snmpTlstmSessionUnknownServerCertificate or
              snmpTlstmSessionInvalidServerCertificates MUST be
              incremented and a snmpTlstmServerCertificateUnknown or
              snmpTlstmServerInvalidCertificate notification SHOULD be
              sent as appropriate.  Reasons for server certificate
              invalidation includes, but is not limited to,
              cryptographic validation failures and an unexpected
              presented certificate identity.
        */

        /* RFCXXXX Section 5.3.1: Establishing a Session as a Client
           4)  The (D)TLS client MUST then verify that the (D)TLS server's
               presented certificate is the expected certificate.  The (D)TLS
               client MUST NOT transmit SNMP messages until the server
               certificate has been authenticated, the client certificate has
               been transmitted and the TLS connection has been fully
               established.

               If the connection is being established from configuration based
               on SNMP-TARGET-MIB configuration, then the snmpTlstmAddrTable
               DESCRIPTION clause describes how the verification is done (using
               either a certificate fingerprint, or an identity authenticated
               via certification path validation).

               If the connection is being established for reasons other than
               configuration found in the SNMP-TARGET-MIB then configuration and
               procedures outside the scope of this document should be followed.
               Configuration mechanisms SHOULD be similar in nature to those
               defined in the snmpTlstmAddrTable to ensure consistency across
               management configuration systems.  For example, a command-line
               tool for generating SNMP GETs might support specifying either the
               server's certificate fingerprint or the expected host name as a
               command line argument.
        */

        /* Implementation notes:
           - All remote certificate fingerprints are expected to be
             stored in the transport's config information.  This is
             true both for CLI clients and TARGET-MIB sessions.
           - netsnmp_tlsbase_verify_server_cert implements these checks
        */
        if (netsnmp_tlsbase_verify_server_cert(ssl, tlsdata) != SNMPERR_SUCCESS) {
            /* XXX: unknown vs invalid; two counters */
            snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONUNKNOWNSERVERCERTIFICATE);
            snmp_log(LOG_ERR, "tlstcp: failed to verify ssl certificate\n");
            SSL_shutdown(ssl);
            BIO_free(bio);
            SNMP_FREE(tlsdata);
            SNMP_FREE(t);
            return NULL;
        }

        /* RFCXXXX Section 5.3.1: Establishing a Session as a Client
           5)  (D)TLS provides assurance that the authenticated identity has
               been signed by a trusted configured certification authority.  If
               verification of the server's certificate fails in any way (for
               example because of failures in cryptographic verification or the
               presented identity did not match the expected named entity) then
               the session establishment MUST fail, the
               snmpTlstmSessionInvalidServerCertificates object is incremented.
               If the session can not be opened for any reason at all, including
               cryptographic verification failures, then the
               snmpTlstmSessionOpenErrors counter is incremented and processing
               stops.

        */
        /* XXX: add snmpTlstmSessionInvalidServerCertificates on
           crypto failure */

        /* RFCXXXX Section 5.3.1: Establishing a Session as a Client
           6)  The TLSTM-specific session identifier (tlstmSessionID) is set in
           the tmSessionID of the tmStateReference passed to the TLS
           Transport Model to indicate that the session has been established
           successfully and to point to a specific (D)TLS connection for
           future use.  The tlstmSessionID is also stored in the LCD for
           later lookup during processing of incoming messages
           (Section 5.1.2).
        */
        /* Implementation notes:
           - the tlsdata pointer is used as our session identifier, as
             noted in the netsnmp_tlstcp_recv() function comments.
        */

        t->sock = BIO_get_fd(bio, NULL);
        /* XXX: save state */
    } else {
        /* Is the server */
        
        /* Create the socket bio */
        snprintf(tmpbuf, sizeof(tmpbuf), "%d", ntohs(tlsdata->addr.sin_port));
        DEBUGMSGTL(("tlstcp", "listening on tlstcp port %s\n", tmpbuf));
        tlsdata->accept_bio = BIO_new_accept(tmpbuf);
        if (NULL == tlsdata->accept_bio) {
            SNMP_FREE(t);
            SNMP_FREE(tlsdata);
            snmp_log(LOG_ERR, "TLSTCP: Falied to create a accept BIO\n");
            return NULL;
        }

        /* openssl requires an initial accept to bind() the socket */
        if (BIO_do_accept(tlsdata->accept_bio) <= 0) {
            SNMP_FREE(t);
            SNMP_FREE(tlsdata);
            snmp_log(LOG_ERR, "TLSTCP: Falied to do first accept on the TLS accept BIO\n");
            return NULL;
        }

        /* create the OpenSSL TLS context */
        tlsdata->ssl_context =
            sslctx_server_setup(TLSv1_method());

        t->sock = BIO_get_fd(tlsdata->accept_bio, NULL);
        t->flags |= NETSNMP_TRANSPORT_FLAG_LISTEN;
    }
    return t;
}

/*
 * Create a TLS-based transport for SNMP.  Local is TRUE if addr is the local
 * address to bind to (i.e. this is a server-type session); otherwise addr is 
 * the remote address to send things to.  
 */

netsnmp_transport *
netsnmp_tlstcp_transport(struct sockaddr_in *addr, int isserver)
{
    netsnmp_transport *t = NULL;
    _netsnmpTLSBaseData *tlsdata;
    
    if (addr == NULL || addr->sin_family != AF_INET) {
        return NULL;
    }

    /* allocate our transport structure */
    t = SNMP_MALLOC_TYPEDEF(netsnmp_transport);
    if (NULL == t) {
        return NULL;
    }
    memset(t, 0, sizeof(netsnmp_transport));

    /* allocate our TLS specific data */
    if (NULL == (tlsdata = netsnmp_tlsbase_allocate_tlsdata(t, isserver)))
        return NULL;

    if (!isserver)
        t->flags |= NETSNMP_TLSBASE_IS_CLIENT;
    memcpy(&tlsdata->addr, addr, sizeof(tlsdata->addr));

    t->data = tlsdata;
    t->data_length = sizeof(_netsnmpTLSBaseData);

    /*
     * Set Domain
     */
    t->domain = netsnmpTLSTCPDomain;                                     
    t->domain_length = netsnmpTLSTCPDomain_len;     

    /*
     * 16-bit length field, 8 byte TLS header, 20 byte IPv4 header  
     */

    t->msgMaxSize      = 0xffff - 8 - 20;
    t->f_recv          = netsnmp_tlstcp_recv;
    t->f_send          = netsnmp_tlstcp_send;
    t->f_open          = netsnmp_tlstcp_open;
    t->f_close         = netsnmp_tlstcp_close;
    t->f_accept        = netsnmp_tlstcp_accept;
    t->f_copy          = netsnmp_tlstcp_copy;
    t->f_config        = netsnmp_tlsbase_config;
    t->f_setup_session = netsnmp_tlsbase_session_init;
    t->f_fmtaddr       = netsnmp_tlstcp_fmtaddr;

    t->flags |= NETSNMP_TRANSPORT_FLAG_TUNNELED | NETSNMP_TRANSPORT_FLAG_STREAM;

    return t;
}

netsnmp_transport *
netsnmp_tlstcp_create_tstring(const char *str, int local,
                               const char *default_target)
{
    struct sockaddr_in addr;

    if (netsnmp_sockaddr_in2(&addr, str, default_target)) {
        return netsnmp_tlstcp_transport(&addr, local);
    } else {
        return NULL;
    }
}


netsnmp_transport *
netsnmp_tlstcp_create_ostring(const u_char * o, size_t o_len, int local)
{
    struct sockaddr_in addr;

    if (o_len == 6) {
        unsigned short porttmp = (o[4] << 8) + o[5];
        addr.sin_family = AF_INET;
        memcpy((u_char *) & (addr.sin_addr.s_addr), o, 4);
        addr.sin_port = htons(porttmp);
        return netsnmp_tlstcp_transport(&addr, local);
    }
    return NULL;
}

void
netsnmp_tlstcp_ctor(void)
{
    DEBUGMSGTL(("tlstcp", "registering TLS constructor\n"));

    /* config settings */

    tlstcpDomain.name = netsnmpTLSTCPDomain;
    tlstcpDomain.name_length = netsnmpTLSTCPDomain_len;
    tlstcpDomain.prefix = (const char**)calloc(2, sizeof(char *));
    tlstcpDomain.prefix[0] = "tlstcp";

    tlstcpDomain.f_create_from_tstring_new = netsnmp_tlstcp_create_tstring;
    tlstcpDomain.f_create_from_ostring = netsnmp_tlstcp_create_ostring;

    netsnmp_tdomain_register(&tlstcpDomain);
}
