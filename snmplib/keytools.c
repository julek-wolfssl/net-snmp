/* Portions of this file are subject to the following copyright(s).  See
 * the Net-SNMP's COPYING file for more details and other copyrights
 * that may apply:
 */
/*
 * Portions of this file are copyrighted by:
 * Copyright � 2003 Sun Microsystems, Inc. All rights reserved.
 * Use is subject to license terms specified in the COPYING file
 * distributed with the Net-SNMP package.
 *
 * Portions of this file are copyrighted by:
 * Copyright (c) 2016 VMware, Inc. All rights reserved.
 * Use is subject to license terms specified in the COPYING file
 * distributed with the Net-SNMP package.
 */

/*
 * keytools.c
 */

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-features.h>

#include <stdio.h>
#include <sys/types.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#include <math.h>

#include <net-snmp/types.h>
#include <net-snmp/output_api.h>
#include <net-snmp/utilities.h>

#include <net-snmp/library/snmp_api.h>
#ifdef NETSNMP_USE_OPENSSL
#	include <openssl/hmac.h>
#else
#ifdef NETSNMP_USE_INTERNAL_MD5
#include <net-snmp/library/md5.h>
#endif
#endif
#ifdef NETSNMP_USE_INTERNAL_CRYPTO
#include <net-snmp/library/openssl_md5.h>
#include <net-snmp/library/openssl_sha.h>
#endif

#ifdef NETSNMP_USE_PKCS11
#include <security/cryptoki.h>
#endif

#include <net-snmp/library/scapi.h>
#include <net-snmp/library/keytools.h>

#include <net-snmp/library/transform_oids.h>

#include <net-snmp/library/snmp_secmod.h>
#include <net-snmp/library/snmpusm.h>

netsnmp_feature_child_of(usm_support, libnetsnmp)
netsnmp_feature_child_of(usm_keytools, usm_support)

#ifndef NETSNMP_FEATURE_REMOVE_USM_KEYTOOLS

/*******************************************************************-o-******
 * generate_Ku
 *
 * Parameters:
 *	*hashtype	MIB OID for the transform type for hashing.
 *	 hashtype_len	Length of OID value.
 *	*P		Pre-allocated bytes of passpharase.
 *	 pplen		Length of passphrase.
 *	*Ku		Buffer to contain Ku.
 *	*kulen		Length of Ku buffer.
 *      
 * Returns:
 *	SNMPERR_SUCCESS			Success.
 *	SNMPERR_GENERR			All errors.
 *
 *
 * Convert a passphrase into a master user key, Ku, according to the
 * algorithm given in RFC 2274 concerning the SNMPv3 User Security Model (USM)
 * as follows:
 *
 * Expand the passphrase to fill the passphrase buffer space, if necessary,
 * concatenation as many duplicates as possible of P to itself.  If P is
 * larger than the buffer space, truncate it to fit.
 *
 * Then hash the result with the given hashtype transform.  Return
 * the result as Ku.
 *
 * If successful, kulen contains the size of the hash written to Ku.
 *
 * NOTE  Passphrases less than USM_LENGTH_P_MIN characters in length
 *	 cause an error to be returned.
 *	 (Punt this check to the cmdline apps?  XXX)
 */
int
generate_Ku(const oid * hashtype, u_int hashtype_len,
            const u_char * P, size_t pplen, u_char * Ku, size_t * kulen)
#if defined(NETSNMP_USE_INTERNAL_MD5) || defined(NETSNMP_USE_OPENSSL) || defined(NETSNMP_USE_INTERNAL_CRYPTO)
{
    int             rval = SNMPERR_SUCCESS,
        nbytes = USM_LENGTH_EXPANDED_PASSPHRASE, auth_type;
#if !defined(NETSNMP_USE_OPENSSL) && \
    defined(NETSNMP_USE_INTERNAL_MD5) || defined(NETSNMP_USE_INTERNAL_CRYPTO)
    int             ret;
#endif

    u_int           i, pindex = 0;

    u_char          buf[USM_LENGTH_KU_HASHBLOCK], *bufp;

#ifdef NETSNMP_USE_OPENSSL
    EVP_MD_CTX     *ctx = NULL;
    const EVP_MD   *hashfn = NULL;
#elif NETSNMP_USE_INTERNAL_CRYPTO
    SHA_CTX csha1;
    MD5_CTX cmd5;
    char    cryptotype = 0;
#define TYPE_MD5  1
#define TYPE_SHA1 2
#else
    MDstruct        MD;
#endif
    /*
     * Sanity check.
     */
    if (!hashtype || !P || !Ku || !kulen || (*kulen <= 0)) {
        QUITFUN(SNMPERR_GENERR, generate_Ku_quit);
    }

    if (pplen < USM_LENGTH_P_MIN) {
        snmp_log(LOG_ERR, "Error: passphrase chosen is below the length "
                 "requirements of the USM (min=%d).\n",USM_LENGTH_P_MIN);
        snmp_set_detail("The supplied password length is too short.");
        QUITFUN(SNMPERR_GENERR, generate_Ku_quit);
    }

    auth_type = sc_get_authtype(hashtype, hashtype_len);
    if (SNMPERR_GENERR == auth_type) {
        snmp_log(LOG_ERR, "Error: unknown authtype");
        snmp_set_detail("unknown authtype");
        QUITFUN(SNMPERR_GENERR, generate_Ku_quit);
    }

    /*
     * Setup for the transform type.
     */
#ifdef NETSNMP_USE_OPENSSL

    if (*kulen < EVP_MAX_MD_SIZE) {
        snmp_log(LOG_ERR, "Internal Error: ku buffer too small (min=%d).\n",
                 EVP_MAX_MD_SIZE);
        snmp_set_detail("Internal Error: ku buffer too small.");
        QUITFUN(SNMPERR_GENERR, generate_Ku_quit);
    }

    /** get hash function */
    hashfn = sc_get_openssl_hashfn(auth_type);
    if (NULL == hashfn) {
        snmp_log(LOG_ERR, "Error: no hashfn for authtype");
        snmp_set_detail("no hashfn for authtype");
        QUITFUN(SNMPERR_GENERR, generate_Ku_quit);
    }

#if defined(HAVE_EVP_MD_CTX_NEW)
    ctx = EVP_MD_CTX_new();
#elif defined(HAVE_EVP_MD_CTX_CREATE)
    ctx = EVP_MD_CTX_create();
#else
    ctx = malloc(sizeof(*ctx));
    if (!EVP_MD_CTX_init(ctx))
        return SNMPERR_GENERR;
#endif
    if (!EVP_DigestInit(ctx, hashfn))
        return SNMPERR_GENERR;

#elif NETSNMP_USE_INTERNAL_CRYPTO
#ifndef NETSNMP_DISABLE_MD5
    if (NETSNMP_USMAUTH_HMACMD5 == auth_type) {
        if (!MD5_Init(&cmd5))
            return SNMPERR_GENERR;
        cryptotype = TYPE_MD5;
    } else
#endif
           if (NETSNMP_USMAUTH_HMACSHA1 == auth_type) {
        if (!SHA1_Init(&csha1))
            return SNMPERR_GENERR;
        cryptotype = TYPE_SHA1;
    } else {
        return (SNMPERR_GENERR);
    }
#else
    MDbegin(&MD);
#endif                          /* NETSNMP_USE_OPENSSL */

    while (nbytes > 0) {
        bufp = buf;
        for (i = 0; i < USM_LENGTH_KU_HASHBLOCK; i++) {
            *bufp++ = P[pindex++ % pplen];
        }
#ifdef NETSNMP_USE_OPENSSL
        EVP_DigestUpdate(ctx, buf, USM_LENGTH_KU_HASHBLOCK);
#elif NETSNMP_USE_INTERNAL_CRYPTO
        if (TYPE_SHA1 == cryptotype) {
            rval = !SHA1_Update(&csha1, buf, USM_LENGTH_KU_HASHBLOCK);
        } else {
            rval = !MD5_Update(&cmd5, buf, USM_LENGTH_KU_HASHBLOCK);
        }
        if (rval != 0) {
            return SNMPERR_USM_ENCRYPTIONERROR;
        }
#elif NETSNMP_USE_INTERNAL_MD5
        if (MDupdate(&MD, buf, USM_LENGTH_KU_HASHBLOCK * 8)) {
            rval = SNMPERR_USM_ENCRYPTIONERROR;
            goto md5_fin;
        }
#endif                          /* NETSNMP_USE_OPENSSL */
        nbytes -= USM_LENGTH_KU_HASHBLOCK;
    }

#ifdef NETSNMP_USE_OPENSSL
    {
    unsigned int    tmp_len;

    tmp_len = *kulen;
    EVP_DigestFinal(ctx, (unsigned char *) Ku, &tmp_len);
    *kulen = tmp_len;
    /*
     * what about free() 
     */
    }
#elif NETSNMP_USE_INTERNAL_CRYPTO
    if (TYPE_SHA1 == cryptotype) {
        SHA1_Final(Ku, &csha1);
    } else {
        MD5_Final(Ku, &cmd5);
    }
    ret = sc_get_properlength(hashtype, hashtype_len);
    if (ret == SNMPERR_GENERR)
        return SNMPERR_GENERR;
    *kulen = ret;
#elif NETSNMP_USE_INTERNAL_MD5
    if (MDupdate(&MD, buf, 0)) {
        rval = SNMPERR_USM_ENCRYPTIONERROR;
        goto md5_fin;
    }
    ret = sc_get_properlength(hashtype, hashtype_len);
    if (ret == SNMPERR_GENERR)
        return SNMPERR_GENERR;
    *kulen = ret;
    MDget(&MD, Ku, *kulen);
  md5_fin:
    memset(&MD, 0, sizeof(MD));
#endif                          /* NETSNMP_USE_INTERNAL_MD5 */


#ifdef NETSNMP_ENABLE_TESTING_CODE
    DEBUGMSGTL(("generate_Ku", "generating Ku (%s from %s): ",
                usm_lookup_auth_str(auth_type), P));
    for (i = 0; i < *kulen; i++)
        DEBUGMSG(("generate_Ku", "%02x", Ku[i]));
    DEBUGMSG(("generate_Ku", "\n"));
#endif                          /* NETSNMP_ENABLE_TESTING_CODE */


  generate_Ku_quit:
    memset(buf, 0, sizeof(buf));
#ifdef NETSNMP_USE_OPENSSL
    if (ctx) {
#if defined(HAVE_EVP_MD_CTX_FREE)
        EVP_MD_CTX_free(ctx);
#elif defined(HAVE_EVP_MD_CTX_DESTROY)
        EVP_MD_CTX_destroy(ctx);
#else
        EVP_MD_CTX_cleanup(ctx);
        free(ctx);
#endif
    }
#endif
    return rval;

}                               /* end generate_Ku() */
#elif NETSNMP_USE_PKCS11
{
    int             rval = SNMPERR_SUCCESS, auth_type;;

    /*
     * Sanity check.
     */
    if (!hashtype || !P || !Ku || !kulen || (*kulen <= 0)) {
        QUITFUN(SNMPERR_GENERR, generate_Ku_quit);
    }

    if (pplen < USM_LENGTH_P_MIN) {
        snmp_log(LOG_ERR, "Error: passphrase chosen is below the length "
                 "requirements of the USM (min=%d).\n",USM_LENGTH_P_MIN);
        snmp_set_detail("The supplied password length is too short.");
        QUITFUN(SNMPERR_GENERR, generate_Ku_quit);
    }

    auth_type = sc_get_authtype(hashtype, hashtype_len);
    if (SNMPERR_GENERR == auth_type) {
        snmp_log(LOG_ERR, "Error: unknown authtype");
        snmp_set_detail("unknown authtype");
        QUITFUN(SNMPERR_GENERR, generate_Ku_quit);
    }


    /*
     * Setup for the transform type.
     */

#ifndef NETSNMP_DISABLE_MD5
    if (NETSNMP_USMAUTH_HMACMD5 == auth_type)
        return pkcs_generate_Ku(CKM_MD5, P, pplen, Ku, kulen);
    else
#endif
        if (NETSNMP_USMAUTH_HMACSHA1 == auth_type)
        return pkcs_generate_Ku(CKM_SHA_1, P, pplen, Ku, kulen);
    else {
        return (SNMPERR_GENERR);
    }

  generate_Ku_quit:

    return rval;
}                               /* end generate_Ku() */
#else
_KEYTOOLS_NOT_AVAILABLE
#endif                          /* internal or openssl */
/*******************************************************************-o-******
 * generate_kul
 *
 * Parameters:
 *	*hashtype
 *	 hashtype_len
 *	*engineID
 *	 engineID_len
 *	*Ku		Master key for a given user.
 *	 ku_len		Length of Ku in bytes.
 *	*Kul		Localized key for a given user at engineID.
 *	*kul_len	Length of Kul buffer (IN); Length of Kul key (OUT).
 *      
 * Returns:
 *	SNMPERR_SUCCESS			Success.
 *	SNMPERR_GENERR			All errors.
 *
 *
 * Ku MUST be the proper length (currently fixed) for the given hashtype.
 *
 * Upon successful return, Kul contains the localized form of Ku at
 * engineID, and the length of the key is stored in kul_len.
 *
 * The localized key method is defined in RFC2274, Sections 2.6 and A.2, and
 * originally documented in:
 *  	U. Blumenthal, N. C. Hien, B. Wijnen,
 *     	"Key Derivation for Network Management Applications",
 *	IEEE Network Magazine, April/May issue, 1997.
 *
 *
 * ASSUMES  SNMP_MAXBUF >= sizeof(Ku + engineID + Ku).
 *
 * NOTE  Localized keys for privacy transforms are generated via
 *	 the authentication transform held by the same usmUser.
 *
 * XXX	An engineID of any length is accepted, even if larger than
 *	what is spec'ed for the textual convention.
 */
int
generate_kul(const oid * hashtype, u_int hashtype_len,
             const u_char * engineID, size_t engineID_len,
             const u_char * Ku, size_t ku_len,
             u_char * Kul, size_t * kul_len)
#if defined(NETSNMP_USE_OPENSSL) || defined(NETSNMP_USE_INTERNAL_MD5) || defined(NETSNMP_USE_PKCS11) || defined(NETSNMP_USE_INTERNAL_CRYPTO)
{
    int             rval = SNMPERR_SUCCESS, auth_type;
    u_int           nbytes = 0;
    size_t          properlength;
    int             iproperlength;

    u_char          buf[SNMP_MAXBUF];
#ifdef NETSNMP_ENABLE_TESTING_CODE
    int             i;
#endif


    /*
     * Sanity check.
     */
    if (!hashtype || !engineID || !Ku || !Kul || !kul_len
        || (engineID_len <= 0) || (ku_len <= 0) || (*kul_len <= 0)) {
        QUITFUN(SNMPERR_GENERR, generate_kul_quit);
    }

    auth_type = sc_get_authtype(hashtype, hashtype_len);
    if (SNMPERR_GENERR == auth_type)
        QUITFUN(SNMPERR_GENERR, generate_kul_quit);

    iproperlength = sc_get_proper_auth_length_bytype(auth_type);
    if (iproperlength == SNMPERR_GENERR)
        QUITFUN(SNMPERR_GENERR, generate_kul_quit);

    properlength = (size_t) iproperlength;

    if ((*kul_len < properlength) || (ku_len < properlength)) {
        QUITFUN(SNMPERR_GENERR, generate_kul_quit);
    }

    /*
     * Concatenate Ku and engineID properly, then hash the result.
     * Store it in Kul.
     */
    nbytes = 0;
    memcpy(buf, Ku, properlength);
    nbytes += properlength;
    memcpy(buf + nbytes, engineID, engineID_len);
    nbytes += engineID_len;
    memcpy(buf + nbytes, Ku, properlength);
    nbytes += properlength;

    rval = sc_hash(hashtype, hashtype_len, buf, nbytes, Kul, kul_len);

#ifdef NETSNMP_ENABLE_TESTING_CODE
    DEBUGMSGTL(("generate_kul", "generating Kul (%s from Ku): ",
                usm_lookup_auth_str(auth_type) ));
    for (i = 0; i < *kul_len; i++)
        DEBUGMSG(("generate_kul", "%02x", Kul[i]));
    DEBUGMSG(("generate_kul", "keytools\n"));
#endif                          /* NETSNMP_ENABLE_TESTING_CODE */

    QUITFUN(rval, generate_kul_quit);


  generate_kul_quit:
    return rval;

}                               /* end generate_kul() */

#else
_KEYTOOLS_NOT_AVAILABLE
#endif                          /* internal or openssl */

/*******************************************************************-o-******
 * _kul_extend_blumenthal
 *
 * Parameters:
 *     *hashoid        MIB OID for the hash transform type.
 *      hashoid_len    Length of the MIB OID hash transform type.
 *     *origKul        original kul (localized; IN/OUT)
 *     *origKulLen     Length of original kul in bytes (IN/OUT)
 *      origKulSize    Size of original kul buffer
 *      needKeyLen     Size needed for key
 *
 * Returns:
 *      SNMPERR_SUCCESS      Success.
 *      SNMPERR_GENERR       All errors.
 */
#ifdef NETSNMP_DRAFT_BLUMENTHAL_AES_04
static int
_kul_extend_blumenthal(int needKeyLen, oid *hashoid, u_int hashoid_len,
                       u_char *origKul, size_t *origKulLen, int origKulSize)
{
    u_char newKul[USM_LENGTH_KU_HASHBLOCK];
    size_t newKulLen;
    int count, hashBits, hashBytes, authtype, i, saveLen;

    DEBUGMSGTL(("usm:extend_kul", " blumenthal called\n"));

    if (NULL == hashoid || NULL == origKul || NULL == origKulLen ||
        needKeyLen > origKulSize) {
        DEBUGMSGTL(("usm:extend_kul", "bad parameters\n"));
        return SNMPERR_GENERR;
   }

    authtype = sc_get_authtype(hashoid, hashoid_len);
    if (authtype < 0 ) {
        DEBUGMSGTL(("usm:extend_kul", "unknown authtype\n"));
        return SNMPERR_GENERR;
    }

    saveLen = *origKulLen;
    needKeyLen -= *origKulLen; /* subtract bytes we already have */

    /*
     * 3.1.2.1:
     *   1)Let Hnnn() the hash function of the authentication protocol for
     *      the user U on the SNMP authoritative engine E. nnn being the size
     *      of the output of the hash function (e.g. nnn=128 bits for MD5, or
     *      nnn=160 bits for SHA1).
     */
    hashBytes = sc_get_proper_auth_length_bytype(authtype);
    hashBits = 8 * hashBytes;

    /* 3.1.2.1:
     *   2)Set c = ceil ( 256 / nnn )
     */
    count = ceil( 256 / hashBits );
    DEBUGMSGTL(("9:usm:extend_kul:blumenthal", "count ceiling %d\n", count));

    /* 3.1.2.1:
     *   3)For i = 1, 2, ..., c
     *        a.Set Kul = Kul || Hnnn(Kul);     Where Hnnn() is the hash
     *          function of the authentication protocol defined for that user
     */
    for(i = 0; i < count; ++i) {
        int copyLen, rc;

        newKulLen = sizeof(newKul);
        rc = sc_hash_type( authtype, origKul, *origKulLen, newKul, &newKulLen);
        if (SNMPERR_SUCCESS != rc) {
            DEBUGMSGTL(("usm:extend_kul", "error from sc_hash_type\n"));
            return SNMPERR_GENERR;
        }

        copyLen = SNMP_MIN(needKeyLen, newKulLen);
        memcpy(origKul + *origKulLen, newKul, copyLen);
        needKeyLen -= copyLen;
        *origKulLen += copyLen;

        /** not part of the draft, but stop if we already have enought bits */
        if (needKeyLen <= 0)
            break;
    }

    DEBUGMSGTL(("usm:extend_kul:blumenthal",
                "orig len %d, new len %" NETSNMP_PRIz "d\n",
                saveLen, *origKulLen));

    return SNMPERR_SUCCESS;
}
#endif /* NETSNMP_DRAFT_BLUMENTHAL_AES_04 */

/*******************************************************************-o-******
 * _kul_extend_reeder
 *
 * Parameters:
 *     *hashoid        MIB OID for the hash transform type.
 *      hashoid_len    Length of the MIB OID hash transform type.
 *     *engineID       engineID
 *      engineIDLen    Length of engineID
 *     *origKul        original kul (localized; IN/OUT)
 *     *origKulLen     Length of original kul in bytes (IN/OUT)
 *      origKulSize    Size of original kul buffer
 *
 * Returns:
 *      SNMPERR_SUCCESS      Success.
 *      SNMPERR_GENERR       All errors.
 */
#if defined(NETSNMP_DRAFT_REEDER_3DES) || defined(NETSNMP_DRAFT_BLUMENTHAL_AES_04)
static int
_kul_extend_reeder(int needKeyLen, oid *hashoid, u_int hashoid_len,
                   u_char *engineID, int engineIDLen,
                   u_char *origKul, size_t *origKulLen, int origKulSize)
{
    u_char newKu[USM_LENGTH_KU_HASHBLOCK], newKul[USM_LENGTH_KU_HASHBLOCK];
    size_t newKuLen, newKulLen, saveLen;
    int authType, copylen;

    DEBUGMSGTL(("usm:extend_kul", " reeder called\n"));

    if (NULL == hashoid || NULL == engineID || NULL == origKul ||
        NULL == origKulLen || needKeyLen > origKulSize)
        return SNMPERR_GENERR;

    authType = sc_get_authtype(hashoid, hashoid_len);
    if (SNMPERR_GENERR == authType)
        return SNMPERR_GENERR;
    newKulLen = sc_get_proper_auth_length_bytype(authType);
    saveLen = *origKulLen;
    needKeyLen -= *origKulLen; /* subtract bytes we already have */
    while (needKeyLen > 0) {

        newKuLen = sizeof(newKu);
        /*
         * hash the existing key using the passphrase to Ku algorithm.
         * [ Ku' = Ku(kul) ]
         */
        if (generate_Ku(hashoid, hashoid_len, origKul, *origKulLen,
                        newKu, &newKuLen) != SNMPERR_SUCCESS)
            return SNMPERR_GENERR;

        /*
         * localize the new key generated from current localized key
         * and append it to the current localized key.
         * [ kul' = kul || kul(Ku') ]
         */
        newKulLen = sizeof(newKul);
        if(generate_kul(hashoid, hashoid_len, engineID, engineIDLen,
                        newKu, newKuLen, newKul,
                        &newKulLen) != SNMPERR_SUCCESS)
            return SNMPERR_GENERR;

        copylen = SNMP_MIN(needKeyLen, newKulLen);
        memcpy(origKul + *origKulLen, newKul, copylen);
        needKeyLen -= copylen;
        *origKulLen += copylen;
    }

    DEBUGMSGTL(("usm:extend_kul:reeder",
                "orig len %" NETSNMP_PRIz "d, new len %" NETSNMP_PRIz "d\n",
                saveLen, *origKulLen));
    return SNMPERR_SUCCESS;
}
#endif /* NETSNMP_DRAFT_REEDER_3DES || NETSNMP_DRAFT_BLUMENTHAL_AES_04 */

/*******************************************************************-o-******
 * netsnmp_extend_key
 *
 * Extend a kul buffer to the needed key size. if the passed kulBuf
 * is not large enough, a new one will be allocated and the old one
 * will be freed.
 *
 * Parameters:
 *       neededKeyLen   The neede key length
 *      *hashoid        MIB OID for the hash transform type.
 *       hashoid_len    Length of the MIB OID hash transform type.
 *       privType       Privay algorithm type
 *       engineID       engineID
 *       engineIDLen    Length of engineID
 *     **kulBuf         Pointer to a buffer pointer
 *      *kulBufLen      Length of current kul buffer
 *       kulBufSize     Allocated size of current kul buffer
 *
 * OUT:
 *     **kulBuf         New kulBuf pointer (if it needed to be expanded)
 *      *kulBufLen      Length of new kul buffer
 *
 * Returns:
 *     SNMPERR_SUCCESS  Success.
 *     SNMPERR_GENERR   All errors.
 */
int
netsnmp_extend_kul(u_int needKeyLen, oid *hashoid, u_int hashoid_len,
                   int privType, u_char *engineID, u_int engineIDLen,
                   u_char **kulBuf, size_t *kulBufLen, u_int kulBufSize)
{
    int ret;
    u_char *newKul;
    size_t newKulLen;
    /* authType = sc_get_authtype(hashoid, hashoid_len); */

    DEBUGMSGTL(("9:usm:extend_kul", " called\n"));

    if (*kulBufLen >= needKeyLen) {
        DEBUGMSGTL(("usm:extend_kul", " key already big enough\n"));
        return SNMPERR_SUCCESS; /* already have enough key material */
    }

    switch (privType & (USM_PRIV_MASK_ALG | USM_PRIV_MASK_VARIANT)) {
#ifdef NETSNMP_DRAFT_BLUMENTHAL_AES_04
        case USM_CREATE_USER_PRIV_AES192:
        case USM_CREATE_USER_PRIV_AES256:
            break;
#endif
#if defined(NETSNMP_DRAFT_REEDER_3DES) || defined(NETSNMP_DRAFT_BLUMENTHAL_AES_04)
        case USM_CREATE_USER_PRIV_3DES:
            break;
#endif
         default:
            DEBUGMSGTL(("usm:extend_kul",
                        "no extension method defined for priv type 0x%x\n",
                        privType));
            return SNMPERR_SUCCESS;
    }

    DEBUGMSGTL(("usm:extend_kul", " have %" NETSNMP_PRIz "d bytes; need %d\n",
                *kulBufLen, needKeyLen));

    if (kulBufSize < needKeyLen) {
        newKul = calloc(1, needKeyLen);
        if (NULL == newKul)
            return SNMPERR_GENERR;
        memcpy(newKul, *kulBuf, *kulBufLen);
        newKulLen = *kulBufLen;
        kulBufSize = needKeyLen;
    }
    else {
        newKul = *kulBuf;
        newKulLen = *kulBufLen;
    }

#ifdef NETSNMP_ENABLE_TESTING_CODE
    DEBUGIF("9:usm:extend_kul") {
        int             i;
        DEBUGMSG(("9:usm:extend_kul",
                  "key: key=0x"));
        for (i = 0; i < newKulLen; i++)
            DEBUGMSG(("9:usm:extend_kul", "%02x", newKul[i] & 0xff));
        DEBUGMSG(("9:usm:extend_kul", " (%ld)\n", newKulLen));
    }
#endif                          /* NETSNMP_ENABLE_TESTING_CODE */
    ret = SNMPERR_SUCCESS; /* most privTypes don't need extended kul */
    switch (privType & (USM_PRIV_MASK_ALG | USM_PRIV_MASK_VARIANT)) {
#ifdef NETSNMP_DRAFT_BLUMENTHAL_AES_04
        case USM_CREATE_USER_PRIV_AES192:
        case USM_CREATE_USER_PRIV_AES256:
        {
            int reeder = privType & USM_AES_REEDER_FLAG;
            if (reeder)
                ret = _kul_extend_reeder(needKeyLen, hashoid, hashoid_len,
                                         engineID, engineIDLen,
                                         newKul, &newKulLen, kulBufSize);
            else
                ret = _kul_extend_blumenthal(needKeyLen, hashoid, hashoid_len,
                                             newKul, &newKulLen, kulBufSize);
        }
        break;
#endif
#if defined(NETSNMP_DRAFT_REEDER_3DES)
        case USM_CREATE_USER_PRIV_3DES:
            ret = _kul_extend_reeder(needKeyLen, hashoid, hashoid_len,
                                     engineID, engineIDLen,
                                     newKul, &newKulLen, kulBufSize);
            break;
#endif
        default:
            DEBUGMSGTL(("usm:extend_kul",
                        "unknown priv type 0x%x\n", privType));
            ret = SNMPERR_GENERR;
    }

    if (SNMPERR_SUCCESS == ret) {
        *kulBufLen = newKulLen;
        if (newKul != *kulBuf) {
            free(*kulBuf);
            *kulBuf = newKul;
        }
    }
    else {
        if (newKul != *kulBuf)
            free(newKul);
    }
#ifdef NETSNMP_ENABLE_TESTING_CODE
    DEBUGIF("9:usm:extend_kul") {
        int             i;
        DEBUGMSG(("usm:extend_kul",
                  "key: key=0x"));
        for (i = 0; i < newKulLen; i++)
            DEBUGMSG(("usm:extend_kul", "%02x", newKul[i] & 0xff));
        DEBUGMSG(("usm:extend_kul", " (%ld)\n", newKulLen));
    }
#endif                          /* NETSNMP_ENABLE_TESTING_CODE */

    return ret;
}

/*******************************************************************-o-******
 * encode_keychange
 *
 * Parameters:
 *	*hashtype	MIB OID for the hash transform type.
 *	 hashtype_len	Length of the MIB OID hash transform type.
 *	*oldkey		Old key that is used to encodes the new key.
 *	 oldkey_len	Length of oldkey in bytes.
 *	*newkey		New key that is encoded using the old key.
 *	 newkey_len	Length of new key in bytes.
 *	*kcstring	Buffer to contain the KeyChange TC string.
 *	*kcstring_len	Length of kcstring buffer.
 *      
 * Returns:
 *	SNMPERR_SUCCESS			Success.
 *	SNMPERR_GENERR			All errors.
 *
 *
 * Uses oldkey and acquired random bytes to encode newkey into kcstring
 * according to the rules of the KeyChange TC described in RFC 2274, Section 5.
 *
 * Upon successful return, *kcstring_len contains the length of the
 * encoded string.
 *
 * ASSUMES	Old and new key are always equal to each other, although
 *		this may be less than the transform type hash output
 * 		output length (eg, using KeyChange for a DESPriv key when
 *		the user also uses SHA1Auth).  This also implies that the
 *		hash placed in the second 1/2 of the key change string
 *		will be truncated before the XOR'ing when the hash output is 
 *		larger than that 1/2 of the key change string.
 *
 *		*kcstring_len will be returned as exactly twice that same
 *		length though the input buffer may be larger.
 *
 * XXX FIX:     Does not handle varibable length keys.
 * XXX FIX:     Does not handle keys larger than the hash algorithm used.
 */
int
encode_keychange(const oid * hashtype, u_int hashtype_len,
                 u_char * oldkey, size_t oldkey_len,
                 u_char * newkey, size_t newkey_len,
                 u_char * kcstring, size_t * kcstring_len)
#if defined(NETSNMP_USE_OPENSSL) || defined(NETSNMP_USE_INTERNAL_MD5) || defined(NETSNMP_USE_PKCS11) || defined(NETSNMP_USE_INTERNAL_CRYPTO)
{
    int             rval = SNMPERR_SUCCESS, auth_type;
    int             iproperlength;
    size_t          properlength;
    size_t          nbytes = 0;

    u_char         *tmpbuf = NULL;


    /*
     * Sanity check.
     */
    if (!kcstring || !kcstring_len)
	return SNMPERR_GENERR;

    if (!hashtype || !oldkey || !newkey || !kcstring || !kcstring_len
        || (oldkey_len <= 0) || (newkey_len <= 0) || (*kcstring_len <= 0)) {
        QUITFUN(SNMPERR_GENERR, encode_keychange_quit);
    }

    /*
     * Setup for the transform type.
     */
    auth_type = sc_get_authtype(hashtype, hashtype_len);
    iproperlength = sc_get_proper_auth_length_bytype(auth_type);
    if (iproperlength == SNMPERR_GENERR)
        QUITFUN(SNMPERR_GENERR, encode_keychange_quit);

    if ((oldkey_len != newkey_len) || (*kcstring_len < (2 * oldkey_len))) {
        QUITFUN(SNMPERR_GENERR, encode_keychange_quit);
    }

    properlength = SNMP_MIN(oldkey_len, (size_t)iproperlength);
    DEBUGMSGTL(("encode_keychange",
                "oldkey_len %ld, newkey_len %ld, auth_length %ld\n",
                oldkey_len, newkey_len, auth_length));

    /*
     * Use the old key and some random bytes to encode the new key
     * in the KeyChange TC format:
     *      . Get random bytes (store in first half of kcstring),
     *      . Hash (oldkey | random_bytes) (into second half of kcstring),
     *      . XOR hash and newkey (into second half of kcstring).
     *
     * Getting the wrong number of random bytes is considered an error.
     */
    nbytes = properlength;

#if defined(NETSNMP_ENABLE_TESTING_CODE) && defined(RANDOMZEROS)
    memset(kcstring, 0, nbytes);
    DEBUGMSG(("encode_keychange",
              "** Using all zero bits for \"random\" delta of )"
              "the keychange string! **\n"));
#else                           /* !NETSNMP_ENABLE_TESTING_CODE */
    rval = sc_random(kcstring, &nbytes);
    QUITFUN(rval, encode_keychange_quit);
    if (nbytes != properlength) {
        QUITFUN(SNMPERR_GENERR, encode_keychange_quit);
    }
#endif                          /* !NETSNMP_ENABLE_TESTING_CODE */
#ifdef NETSNMP_ENABLE_TESTING_CODE
    DEBUGIF("encode_keychange") {
        int             i;
        DEBUGMSG(("encode_keychange",
                  "rand: key=0x"));
        for (i = 0; i < rand_len; i++)
            DEBUGMSG(("encode_keychange", "%02x", randp[i] & 0xff));
        DEBUGMSG(("encode_keychange", " (%ld)\n", rand_len));
    }
#endif                          /* NETSNMP_ENABLE_TESTING_CODE */

    tmpbuf = (u_char *) malloc(properlength * 2);
    if (tmpbuf) {
        memcpy(tmpbuf, oldkey, properlength);
        memcpy(tmpbuf + properlength, kcstring, properlength);

        *kcstring_len -= properlength;
        rval = sc_hash(hashtype, hashtype_len, tmpbuf, properlength * 2,
                       kcstring + properlength, kcstring_len);

        QUITFUN(rval, encode_keychange_quit);
        DEBUGMSGTL(("encode_keychange", "digest_len %ld\n", digest_len));

        *kcstring_len = (properlength * 2);

        kcstring += properlength;
        nbytes = 0;
        while ((nbytes++) < properlength) {
            *kcstring++ ^= *newkey++;
        }
    }

#ifdef NETSNMP_ENABLE_TESTING_CODE
    DEBUGIF("encode_keychange") {
        int             i;
        DEBUGMSG(("encode_keychange",
                  "oldkey: key=0x"));
        for (i = 0; i < oldkey_len; i++)
            DEBUGMSG(("encode_keychange", "%02x", oldkey[i] & 0xff));
        DEBUGMSG(("encode_keychange", " (%ld)\n", oldkey_len));

        DEBUGMSG(("encode_keychange",
                  "newkey: key=0x"));
        for (i = 0; i < newkey_len; i++)
            DEBUGMSG(("encode_keychange", "%02x", newkey[i] & 0xff));
        DEBUGMSG(("encode_keychange", " (%ld)\n", newkey_len));

        DEBUGMSG(("encode_keychange",
                  "kcstring: key=0x"));
        for (i = 0; i < *kcstring_len; i++)
            DEBUGMSG(("encode_keychange", "%02x", kcstring[i] & 0xff));
        DEBUGMSG(("encode_keychange", " (%ld)\n", *kcstring_len));
    }
#endif                          /* NETSNMP_ENABLE_TESTING_CODE */
  encode_keychange_quit:
    if (rval != SNMPERR_SUCCESS)
        memset(kcstring, 0, *kcstring_len);
    SNMP_FREE(tmpbuf);

    return rval;

}                               /* end encode_keychange() */

#else
_KEYTOOLS_NOT_AVAILABLE
#endif                          /* internal or openssl */
/*******************************************************************-o-******
 * decode_keychange
 *
 * Parameters:
 *	*hashtype	MIB OID of the hash transform to use.
 *	 hashtype_len	Length of the hash transform MIB OID.
 *	*oldkey		Old key that is used to encode the new key.
 *	 oldkey_len	Length of oldkey in bytes.
 *	*kcstring	Encoded KeyString buffer containing the new key.
 *	 kcstring_len	Length of kcstring in bytes.
 *	*newkey		Buffer to hold the extracted new key.
 *	*newkey_len	Length of newkey in bytes.
 *      
 * Returns:
 *	SNMPERR_SUCCESS			Success.
 *	SNMPERR_GENERR			All errors.
 *
 *
 * Decodes a string of bits encoded according to the KeyChange TC described
 * in RFC 2274, Section 5.  The new key is extracted from *kcstring with
 * the aid of the old key.
 *
 * Upon successful return, *newkey_len contains the length of the new key.
 *
 *
 * ASSUMES	Old key is exactly 1/2 the length of the KeyChange buffer,
 *		although this length may be less than the hash transform
 *		output.  Thus the new key length will be equal to the old
 *		key length.
 */
/*
 * XXX:  if the newkey is not long enough, it should be freed and remalloced 
 */
int
decode_keychange(const oid * hashtype, u_int hashtype_len,
                 u_char * oldkey, size_t oldkey_len,
                 u_char * kcstring, size_t kcstring_len,
                 u_char * newkey, size_t * newkey_len)
#if defined(NETSNMP_USE_OPENSSL) || defined(NETSNMP_USE_INTERNAL_MD5) || defined(NETSNMP_USE_PKCS11) || defined(NETSNMP_USE_INTERNAL_CRYPTO)
{
    int             rval = SNMPERR_SUCCESS, auth_type;
    size_t          properlength = 0;
    int             iproperlength = 0;
    u_int           nbytes = 0;

    u_char         *bufp, tmp_buf[SNMP_MAXBUF];
    size_t          tmp_buf_len = SNMP_MAXBUF;
    u_char         *tmpbuf = NULL;



    /*
     * Sanity check.
     */
    if (!hashtype || !oldkey || !kcstring || !newkey || !newkey_len
        || (oldkey_len <= 0) || (kcstring_len <= 0) || (*newkey_len <= 0)) {
        DEBUGMSGTL(("decode_keychange", "bad args\n"));
        QUITFUN(SNMPERR_GENERR, decode_keychange_quit);
    }


    /*
     * Setup for the transform type.
     */
    auth_type = sc_get_authtype(hashtype, hashtype_len);
    iproperlength = sc_get_proper_auth_length_bytype(auth_type);
    if (iproperlength == SNMPERR_GENERR)
        DEBUGMSGTL(("decode_keychange", "proper length err\n"));
        QUITFUN(SNMPERR_GENERR, decode_keychange_quit);

    properlength = (size_t) iproperlength;
    DEBUGMSGTL(("usm:decode_keychange",
                "oldkey_len %ld, newkey_len %ld, kcstring_len %ld, hash_len %ld\n",
                oldkey_len, *newkey_len, kcstring_len, hash_len));

    if (((oldkey_len * 2) != kcstring_len) || (*newkey_len < oldkey_len)) {
        DEBUGMSGTL(("decode_keychange", "keylen error\n"));
        QUITFUN(SNMPERR_GENERR, decode_keychange_quit);
    }

    properlength = oldkey_len;
    *newkey_len = properlength;
    {
#ifdef NETSNMP_ENABLE_TESTING_CODE
    DEBUGIF("decode_keychange") {
        int             i;
        DEBUGMSG(("decode_keychange",
                  "oldkey: key=0x"));
        for (i = 0; i < oldkey_len; i++)
            DEBUGMSG(("decode_keychange", "%02x", oldkey[i] & 0xff));
        DEBUGMSG(("decode_keychange", " (%ld)\n", oldkey_len));

        DEBUGMSG(("decode_keychange",
                  "kcstring: key=0x"));
        for (i = 0; i < kcstring_len; i++)
            DEBUGMSG(("decode_keychange", "%02x", kcstring[i] & 0xff));
        DEBUGMSG(("decode_keychange", " (%ld)\n", kcstring_len));
    }
#endif                          /* NETSNMP_ENABLE_TESTING_CODE */

    /*
     * Use the old key and the given KeyChange TC string to recover
     * the new key:
     *      . Hash (oldkey | random_bytes) (into newkey),
     *      . XOR hash and encoded (second) half of kcstring (into newkey).
     */
    tmpbuf = (u_char *) malloc(properlength * 2);
    if (tmpbuf) {
        memcpy(tmpbuf, oldkey, properlength);
        memcpy(tmpbuf + properlength, kcstring, properlength);

        rval = sc_hash(hashtype, hashtype_len, tmpbuf, properlength * 2,
                       tmp_buf, &tmp_buf_len);
        QUITFUN(rval, decode_keychange_quit);

        memcpy(newkey, tmp_buf, properlength);
        bufp = kcstring + properlength;
        nbytes = 0;
        while ((nbytes++) < properlength) {
            *newkey++ ^= *bufp++;
        }
    }

#ifdef NETSNMP_ENABLE_TESTING_CODE
    DEBUGIF("decode_keychange") {
        int             i;
        DEBUGMSG(("decode_keychange",
                  "newkey: key=0x"));
        for (i = 0; i < *newkey_len; i++)
            DEBUGMSG(("decode_keychange", "%02x", newkey_save[i] & 0xff));
        DEBUGMSG(("decode_keychange", " (%ld)\n", *newkey_len));
    }
#endif                          /* NETSNMP_ENABLE_TESTING_CODE */

  decode_keychange_quit:
    if (rval != SNMPERR_SUCCESS) {
        DEBUGMSGTL(("decode_keychange", "error %d\n", rval));
        if (newkey)
            memset(newkey, 0, properlength);
    }
    memset(tmp_buf, 0, SNMP_MAXBUF);
    SNMP_FREE(tmpbuf);

    return rval;

}                               /* end decode_keychange() */

#else
_KEYTOOLS_NOT_AVAILABLE
#endif                          /* internal or openssl */
#endif /* NETSNMP_FEATURE_REMOVE_USM_KEYTOOLS */
