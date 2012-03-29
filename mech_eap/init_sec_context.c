/*
 * Copyright (c) 2011, JANET(UK)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of JANET(UK) nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Establish a security context on the initiator (client). These functions
 * wrap around libeap.
 */

#include "gssapiP_eap.h"

#include <libxml/xmlreader.h>
#include <curl/curl.h>


#ifdef MECH_EAP

static OM_uint32
policyVariableToFlag(enum eapol_bool_var variable)
{
    OM_uint32 flag = 0;

    switch (variable) {
    case EAPOL_eapSuccess:
        flag = CTX_FLAG_EAP_SUCCESS;
        break;
    case EAPOL_eapRestart:
        flag = CTX_FLAG_EAP_RESTART;
        break;
    case EAPOL_eapFail:
        flag = CTX_FLAG_EAP_FAIL;
        break;
    case EAPOL_eapResp:
        flag = CTX_FLAG_EAP_RESP;
        break;
    case EAPOL_eapNoResp:
        flag = CTX_FLAG_EAP_NO_RESP;
        break;
    case EAPOL_eapReq:
        flag = CTX_FLAG_EAP_REQ;
        break;
    case EAPOL_portEnabled:
        flag = CTX_FLAG_EAP_PORT_ENABLED;
        break;
    case EAPOL_altAccept:
        flag = CTX_FLAG_EAP_ALT_ACCEPT;
        break;
    case EAPOL_altReject:
        flag = CTX_FLAG_EAP_ALT_REJECT;
        break;
    }

    return flag;
}

static struct eap_peer_config *
peerGetConfig(void *ctx)
{
    gss_ctx_id_t gssCtx = (gss_ctx_id_t)ctx;

    return &gssCtx->initiatorCtx.eapPeerConfig;
}

static Boolean
peerGetBool(void *data, enum eapol_bool_var variable)
{
    gss_ctx_id_t ctx = data;
    OM_uint32 flag;

    if (ctx == GSS_C_NO_CONTEXT)
        return FALSE;

    flag = policyVariableToFlag(variable);

    return ((ctx->flags & flag) != 0);
}

static void
peerSetBool(void *data, enum eapol_bool_var variable,
            Boolean value)
{
    gss_ctx_id_t ctx = data;
    OM_uint32 flag;

    if (ctx == GSS_C_NO_CONTEXT)
        return;

    flag = policyVariableToFlag(variable);

    if (value)
        ctx->flags |= flag;
    else
        ctx->flags &= ~(flag);
}

static unsigned int
peerGetInt(void *data, enum eapol_int_var variable)
{
    gss_ctx_id_t ctx = data;

    if (ctx == GSS_C_NO_CONTEXT)
        return FALSE;

    GSSEAP_ASSERT(CTX_IS_INITIATOR(ctx));

    switch (variable) {
    case EAPOL_idleWhile:
        return ctx->initiatorCtx.idleWhile;
        break;
    }

    return 0;
}

static void
peerSetInt(void *data, enum eapol_int_var variable,
           unsigned int value)
{
    gss_ctx_id_t ctx = data;

    if (ctx == GSS_C_NO_CONTEXT)
        return;

    GSSEAP_ASSERT(CTX_IS_INITIATOR(ctx));

    switch (variable) {
    case EAPOL_idleWhile:
        ctx->initiatorCtx.idleWhile = value;
        break;
    }
}

static struct wpabuf *
peerGetEapReqData(void *ctx)
{
    gss_ctx_id_t gssCtx = (gss_ctx_id_t)ctx;

    return &gssCtx->initiatorCtx.reqData;
}

static void
peerSetConfigBlob(void *ctx GSSEAP_UNUSED,
                  struct wpa_config_blob *blob GSSEAP_UNUSED)
{
}

static const struct wpa_config_blob *
peerGetConfigBlob(void *ctx GSSEAP_UNUSED,
                  const char *name GSSEAP_UNUSED)
{
    return NULL;
}

static void
peerNotifyPending(void *ctx GSSEAP_UNUSED)
{
}

static struct eapol_callbacks gssEapPolicyCallbacks = {
    peerGetConfig,
    peerGetBool,
    peerSetBool,
    peerGetInt,
    peerSetInt,
    peerGetEapReqData,
    peerSetConfigBlob,
    peerGetConfigBlob,
    peerNotifyPending,
};

#ifdef GSSEAP_DEBUG
extern int wpa_debug_level;
#endif

static OM_uint32
peerConfigInit(OM_uint32 *minor, gss_ctx_id_t ctx)
{
    OM_uint32 major;
    krb5_context krbContext;
    struct eap_peer_config *eapPeerConfig = &ctx->initiatorCtx.eapPeerConfig;
    gss_buffer_desc identity = GSS_C_EMPTY_BUFFER;
    gss_buffer_desc realm = GSS_C_EMPTY_BUFFER;
    gss_cred_id_t cred = ctx->cred;

    eapPeerConfig->identity = NULL;
    eapPeerConfig->identity_len = 0;
    eapPeerConfig->anonymous_identity = NULL;
    eapPeerConfig->anonymous_identity_len = 0;
    eapPeerConfig->password = NULL;
    eapPeerConfig->password_len = 0;

    GSSEAP_ASSERT(cred != GSS_C_NO_CREDENTIAL);

    GSSEAP_KRB_INIT(&krbContext);

    eapPeerConfig->fragment_size = 1024;
#ifdef GSSEAP_DEBUG
    wpa_debug_level = 0;
#endif

    GSSEAP_ASSERT(cred->name != GSS_C_NO_NAME);

    if ((cred->name->flags & (NAME_FLAG_NAI | NAME_FLAG_SERVICE)) == 0) {
        *minor = GSSEAP_BAD_INITIATOR_NAME;
        return GSS_S_BAD_NAME;
    }

    /* identity */
    major = gssEapDisplayName(minor, cred->name, &identity, NULL);
    if (GSS_ERROR(major))
        return major;

    eapPeerConfig->identity = (unsigned char *)identity.value;
    eapPeerConfig->identity_len = identity.length;

    krbPrincRealmToGssBuffer(cred->name->krbPrincipal, &realm);

    /* anonymous_identity */
    eapPeerConfig->anonymous_identity = GSSEAP_MALLOC(realm.length + 2);
    if (eapPeerConfig->anonymous_identity == NULL) {
        *minor = ENOMEM;
        return GSS_S_FAILURE;
    }

    eapPeerConfig->anonymous_identity[0] = '@';
    memcpy(eapPeerConfig->anonymous_identity + 1, realm.value, realm.length);
    eapPeerConfig->anonymous_identity[1 + realm.length] = '\0';
    eapPeerConfig->anonymous_identity_len = 1 + realm.length;

    /* password */
    eapPeerConfig->password = (unsigned char *)cred->password.value;
    eapPeerConfig->password_len = cred->password.length;

    /* certs */
    eapPeerConfig->ca_cert = (unsigned char *)cred->caCertificate.value;
    eapPeerConfig->subject_match = (unsigned char *)cred->subjectNameConstraint.value;
    eapPeerConfig->altsubject_match = (unsigned char *)cred->subjectAltNameConstraint.value;

    *minor = 0;
    return GSS_S_COMPLETE;
}

static OM_uint32
peerConfigFree(OM_uint32 *minor,
               gss_ctx_id_t ctx)
{
    struct eap_peer_config *eapPeerConfig = &ctx->initiatorCtx.eapPeerConfig;

    if (eapPeerConfig->identity != NULL) {
        GSSEAP_FREE(eapPeerConfig->identity);
        eapPeerConfig->identity = NULL;
        eapPeerConfig->identity_len = 0;
    }

    if (eapPeerConfig->anonymous_identity != NULL) {
        GSSEAP_FREE(eapPeerConfig->anonymous_identity);
        eapPeerConfig->anonymous_identity = NULL;
        eapPeerConfig->anonymous_identity_len = 0;
    }

    *minor = 0;
    return GSS_S_COMPLETE;
}

/*
 * Mark an initiator context as ready for cryptographic operations
 */
static OM_uint32
initReady(OM_uint32 *minor, gss_ctx_id_t ctx, OM_uint32 reqFlags)
{
    OM_uint32 major;
    const unsigned char *key;
    size_t keyLength;

#if 1
    /* XXX actually check for mutual auth */
    if (reqFlags & GSS_C_MUTUAL_FLAG)
        ctx->gssFlags |= GSS_C_MUTUAL_FLAG;
#endif

    /* Cache encryption type derived from selected mechanism OID */
    major = gssEapOidToEnctype(minor, ctx->mechanismUsed, &ctx->encryptionType);
    if (GSS_ERROR(major))
        return major;

    if (!eap_key_available(ctx->initiatorCtx.eap)) {
        *minor = GSSEAP_KEY_UNAVAILABLE;
        return GSS_S_UNAVAILABLE;
    }

    key = eap_get_eapKeyData(ctx->initiatorCtx.eap, &keyLength);

    if (keyLength < EAP_EMSK_LEN) {
        *minor = GSSEAP_KEY_TOO_SHORT;
        return GSS_S_UNAVAILABLE;
    }

    major = gssEapDeriveRfc3961Key(minor,
                                   &key[EAP_EMSK_LEN / 2],
                                   EAP_EMSK_LEN / 2,
                                   ctx->encryptionType,
                                   &ctx->rfc3961Key);
       if (GSS_ERROR(major))
           return major;

    major = rfc3961ChecksumTypeForKey(minor, &ctx->rfc3961Key,
                                      &ctx->checksumType);
    if (GSS_ERROR(major))
        return major;

    major = sequenceInit(minor,
                         &ctx->seqState,
                         ctx->recvSeq,
                         ((ctx->gssFlags & GSS_C_REPLAY_FLAG) != 0),
                         ((ctx->gssFlags & GSS_C_SEQUENCE_FLAG) != 0),
                         TRUE);
    if (GSS_ERROR(major))
        return major;

    *minor = 0;
    return GSS_S_COMPLETE;
}
#endif

static OM_uint32
initBegin(OM_uint32 *minor,
          gss_ctx_id_t ctx,
          gss_name_t target,
          gss_OID mech,
          OM_uint32 reqFlags GSSEAP_UNUSED,
          OM_uint32 timeReq,
          gss_channel_bindings_t chanBindings GSSEAP_UNUSED)
{
    OM_uint32 major;
    gss_cred_id_t cred = ctx->cred;

    GSSEAP_ASSERT(cred != GSS_C_NO_CREDENTIAL);

    if (cred->expiryTime)
        ctx->expiryTime = cred->expiryTime;
    else if (timeReq == 0 || timeReq == GSS_C_INDEFINITE)
        ctx->expiryTime = 0;
    else
        ctx->expiryTime = time(NULL) + timeReq;

    /*
     * The credential mutex protects its name, however we need to
     * explicitly lock the acceptor name (unlikely as it may be
     * that it has attributes set on it).
     */
    major = gssEapDuplicateName(minor, cred->name, &ctx->initiatorName);
    if (GSS_ERROR(major))
        return major;

    if (target != GSS_C_NO_NAME) {
        GSSEAP_MUTEX_LOCK(&target->mutex);

        major = gssEapDuplicateName(minor, target, &ctx->acceptorName);
        if (GSS_ERROR(major)) {
            GSSEAP_MUTEX_UNLOCK(&target->mutex);
            return major;
        }

        GSSEAP_MUTEX_UNLOCK(&target->mutex);
    }

    major = gssEapCanonicalizeOid(minor,
                                  mech,
                                  OID_FLAG_NULL_VALID | OID_FLAG_MAP_NULL_TO_DEFAULT_MECH,
                                  &ctx->mechanismUsed);
    if (GSS_ERROR(major))
        return major;

    /* If credentials were provided, check they're usable with this mech */
    if (!gssEapCredAvailable(cred, ctx->mechanismUsed)) {
        *minor = GSSEAP_CRED_MECH_MISMATCH;
        return GSS_S_BAD_MECH;
    }

    *minor = 0;
    return GSS_S_COMPLETE;
}

static OM_uint32
eapGssSmInitError(OM_uint32 *minor,
                  gss_cred_id_t cred GSSEAP_UNUSED,
                  gss_ctx_id_t ctx GSSEAP_UNUSED,
                  gss_name_t target GSSEAP_UNUSED,
                  gss_OID mech GSSEAP_UNUSED,
                  OM_uint32 reqFlags GSSEAP_UNUSED,
                  OM_uint32 timeReq GSSEAP_UNUSED,
                  gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                  gss_buffer_t inputToken,
                  gss_buffer_t outputToken GSSEAP_UNUSED,
                  OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major;
    unsigned char *p;

    if (inputToken->length < 8) {
        *minor = GSSEAP_TOK_TRUNC;
        return GSS_S_DEFECTIVE_TOKEN;
    }

    p = (unsigned char *)inputToken->value;

    major = load_uint32_be(&p[0]);
    *minor = ERROR_TABLE_BASE_eapg + load_uint32_be(&p[4]);

    if (!GSS_ERROR(major) || !IS_WIRE_ERROR(*minor)) {
        major = GSS_S_FAILURE;
        *minor = GSSEAP_BAD_ERROR_TOKEN;
    }

    GSSEAP_ASSERT(GSS_ERROR(major));

    return major;
}

#ifdef GSSEAP_ENABLE_REAUTH
static OM_uint32
eapGssSmInitGssReauth(OM_uint32 *minor,
                      gss_cred_id_t cred,
                      gss_ctx_id_t ctx,
                      gss_name_t target,
                      gss_OID mech GSSEAP_UNUSED,
                      OM_uint32 reqFlags,
                      OM_uint32 timeReq,
                      gss_channel_bindings_t chanBindings,
                      gss_buffer_t inputToken,
                      gss_buffer_t outputToken,
                      OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major, tmpMinor;
    gss_name_t mechTarget = GSS_C_NO_NAME;
    gss_OID actualMech = GSS_C_NO_OID;
    OM_uint32 gssFlags, timeRec;

    /*
     * Here we use the passed in credential handle because the resolved
     * context credential does not currently have the reauth creds.
     */
    if (GSSEAP_SM_STATE(ctx) == GSSEAP_STATE_INITIAL) {
        if (!gssEapCanReauthP(cred, target, timeReq))
            return GSS_S_CONTINUE_NEEDED;

        ctx->flags |= CTX_FLAG_KRB_REAUTH;
    } else if ((ctx->flags & CTX_FLAG_KRB_REAUTH) == 0) {
        major = GSS_S_DEFECTIVE_TOKEN;
        *minor = GSSEAP_WRONG_ITOK;
        goto cleanup;
    }

    GSSEAP_ASSERT(cred != GSS_C_NO_CREDENTIAL);

    major = gssEapMechToGlueName(minor, target, &mechTarget);
    if (GSS_ERROR(major))
        goto cleanup;

    major = gssInitSecContext(minor,
                              cred->reauthCred,
                              &ctx->reauthCtx,
                              mechTarget,
                              (gss_OID)gss_mech_krb5,
                              reqFlags | GSS_C_MUTUAL_FLAG,
                              timeReq,
                              chanBindings,
                              inputToken,
                              &actualMech,
                              outputToken,
                              &gssFlags,
                              &timeRec);
    if (GSS_ERROR(major))
        goto cleanup;

    ctx->gssFlags = gssFlags;

    if (major == GSS_S_COMPLETE) {
        GSSEAP_ASSERT(GSSEAP_SM_STATE(ctx) == GSSEAP_STATE_REAUTHENTICATE);

        major = gssEapReauthComplete(minor, ctx, cred, actualMech, timeRec);
        if (GSS_ERROR(major))
            goto cleanup;
        GSSEAP_SM_TRANSITION(ctx, GSSEAP_STATE_ESTABLISHED);
    } else {
        GSSEAP_SM_TRANSITION(ctx, GSSEAP_STATE_REAUTHENTICATE);
    }

cleanup:
    gssReleaseName(&tmpMinor, &mechTarget);

    return major;
}
#endif /* GSSEAP_ENABLE_REAUTH */

#ifdef GSSEAP_DEBUG
static OM_uint32
eapGssSmInitVendorInfo(OM_uint32 *minor,
                       gss_cred_id_t cred GSSEAP_UNUSED,
                       gss_ctx_id_t ctx GSSEAP_UNUSED,
                       gss_name_t target GSSEAP_UNUSED,
                       gss_OID mech GSSEAP_UNUSED,
                       OM_uint32 reqFlags GSSEAP_UNUSED,
                       OM_uint32 timeReq GSSEAP_UNUSED,
                       gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                       gss_buffer_t inputToken GSSEAP_UNUSED,
                       gss_buffer_t outputToken,
                       OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major;

    major = makeStringBuffer(minor, "JANET(UK)", outputToken);
    if (GSS_ERROR(major))
        return major;

    return GSS_S_CONTINUE_NEEDED;
}
#endif

static OM_uint32
eapGssSmInitAcceptorName(OM_uint32 *minor,
                         gss_cred_id_t cred GSSEAP_UNUSED,
                         gss_ctx_id_t ctx,
                         gss_name_t target GSSEAP_UNUSED,
                         gss_OID mech GSSEAP_UNUSED,
                         OM_uint32 reqFlags GSSEAP_UNUSED,
                         OM_uint32 timeReq GSSEAP_UNUSED,
                         gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                         gss_buffer_t inputToken GSSEAP_UNUSED,
                         gss_buffer_t outputToken,
                         OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major;

    if (GSSEAP_SM_STATE(ctx) == GSSEAP_STATE_INITIAL &&
        ctx->acceptorName != GSS_C_NO_NAME) {

        /* Send desired target name to acceptor */
        major = gssEapDisplayName(minor, ctx->acceptorName,
                                  outputToken, NULL);
        if (GSS_ERROR(major))
            return major;
    } else if (inputToken != GSS_C_NO_BUFFER &&
               ctx->acceptorName == GSS_C_NO_NAME) {
        /* Accept target name hint from acceptor */
        major = gssEapImportName(minor, inputToken,
                                 GSS_C_NT_USER_NAME,
                                 ctx->mechanismUsed,
                                 &ctx->acceptorName);
        if (GSS_ERROR(major))
            return major;
    }

    /*
     * Currently, other parts of the code assume that the acceptor name
     * is available, hence this check.
     */
    if (ctx->acceptorName == GSS_C_NO_NAME) {
        *minor = GSSEAP_NO_ACCEPTOR_NAME;
        return GSS_S_FAILURE;
    }

    return GSS_S_CONTINUE_NEEDED;
}

static OM_uint32
eapGssSmInitIdentity(OM_uint32 *minor,
                     gss_cred_id_t cred GSSEAP_UNUSED,
                     gss_ctx_id_t ctx,
                     gss_name_t target GSSEAP_UNUSED,
                     gss_OID mech GSSEAP_UNUSED,
                     OM_uint32 reqFlags GSSEAP_UNUSED,
                     OM_uint32 timeReq GSSEAP_UNUSED,
                     gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                     gss_buffer_t inputToken GSSEAP_UNUSED,
                     gss_buffer_t outputToken GSSEAP_UNUSED,
                     OM_uint32 *smFlags)
{
    struct eap_config eapConfig;

#ifdef GSSEAP_ENABLE_REAUTH
    if (GSSEAP_SM_STATE(ctx) == GSSEAP_STATE_REAUTHENTICATE) {
        OM_uint32 tmpMinor;

        /* server didn't support reauthentication, sent EAP request */
        gssDeleteSecContext(&tmpMinor, &ctx->reauthCtx, GSS_C_NO_BUFFER);
        ctx->flags &= ~(CTX_FLAG_KRB_REAUTH);
        GSSEAP_SM_TRANSITION(ctx, GSSEAP_STATE_INITIAL);
    } else
#endif
        *smFlags |= SM_FLAG_FORCE_SEND_TOKEN;

    GSSEAP_ASSERT((ctx->flags & CTX_FLAG_KRB_REAUTH) == 0);
    GSSEAP_ASSERT(inputToken == GSS_C_NO_BUFFER);

#ifdef MECH_EAP
    memset(&eapConfig, 0, sizeof(eapConfig));

    ctx->initiatorCtx.eap = eap_peer_sm_init(ctx,
                                             &gssEapPolicyCallbacks,
                                             ctx,
                                             &eapConfig);
    if (ctx->initiatorCtx.eap == NULL) {
        *minor = GSSEAP_PEER_SM_INIT_FAILURE;
        return GSS_S_FAILURE;
    }

    ctx->flags |= CTX_FLAG_EAP_RESTART | CTX_FLAG_EAP_PORT_ENABLED;

    /* poke EAP state machine */
    if (eap_peer_sm_step(ctx->initiatorCtx.eap) != 0) {
        *minor = GSSEAP_PEER_SM_STEP_FAILURE;
        return GSS_S_FAILURE;
    }
#else
    /* makeStringBuffer(minor, "n,,", outputToken); */
#endif

    GSSEAP_SM_TRANSITION_NEXT(ctx);

    *minor = 0;

    return GSS_S_CONTINUE_NEEDED;
}


static xmlNode *
getElement(xmlNode *a_node, char *name)
{
    xmlNode *cur_node = NULL;
    xmlNode *ret_node = NULL;

    for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
            printf("node type: %d, name: %s (%s)\n", cur_node->type, cur_node->name, cur_node->content?(cur_node->content):"");
        if (cur_node->type == XML_ELEMENT_NODE && !strcmp(cur_node->name, name)) {
                return cur_node;
        }

        ret_node = getElement(cur_node->children, name);
        if (ret_node)
            return ret_node;
    }
    return NULL;
}

static void
freeChildren(xmlNode *a_node)
{
    xmlNode *cur_node = NULL;

    cur_node = a_node->children;
    while (cur_node) {
          xmlNode *next_node = cur_node->next;
          xmlUnlinkNode(cur_node);
          xmlFreeNode(cur_node);          cur_node = next_node;
    }
}

char http_data[15096] = ""; /* TODO VSY Make this dynamic */

size_t
write_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
    int numbytes = size * nmemb;
    int http_len = strlen(http_data);

    memcpy(http_data + http_len, buffer, numbytes);
    http_data[http_len + numbytes] = '\0';

    return numbytes;
}

void
sendToIdP(xmlDocPtr doc, char *user, char *password)
{
    CURL *curl;
    CURLcode res;
    xmlChar *mem = NULL;
    int size;
    char userpw[514]; /* TODO VSY: fix this */

    xmlDocDumpFormatMemory(doc, &mem, &size, 0);
    fprintf(stderr, "FORMATTED DOC IS (%p)(%d)\n", mem, size);

    curl = curl_easy_init();
    if (curl && mem && size) {
fprintf(stderr, "DOING HTTP POST\n");
       /* TODO VSY: Use configured IdP */
       curl_easy_setopt(curl, CURLOPT_URL, "https://boingo.ncsa.uiuc.edu/idp/profile/SAML2/SOAP/ECP");

        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        sprintf(userpw, "%s:%s", user, password);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpw);

        curl_easy_setopt(curl, CURLOPT_POST, 1);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, mem);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, size);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);

        res = curl_easy_perform(curl);

        /* always cleanup */
        curl_easy_cleanup(curl);
    }
}


char *
processSAMLRequest(gss_cred_id_t cred, gss_buffer_t request)
{
    char *user = "ysvenkat"; /* TODO: grab this from cred */
    char *password = cred->password.value;
    xmlDocPtr doc_fromsp;
    xmlDocPtr doc_fromidp;
    xmlNode *header_fromsp;

fprintf(stderr, "PASSWORD IS (%s)\n", password);

    doc_fromsp = xmlReadMemory(request->value, request->length, "FROMSP", NULL, 0);
    if (doc_fromsp != NULL)
        xmlDocDump(stdout, doc_fromsp);
    else
        return NULL;

    /* Exclude header */
    header_fromsp = getElement(xmlDocGetRootElement(doc_fromsp), "Header");
    xmlUnlinkNode(header_fromsp);
    xmlDocDump(stdout, doc_fromsp);

    /* Send doc to IdP */
    /* TODO: Error checking here and elsewhere */
    sendToIdP(doc_fromsp, user, password);

fprintf(stdout, "RECEIVED FROM IDP (%s)\n", http_data);

    /* Empty header from IdP and populate with RelayState from
 *     header received from SP */
    doc_fromidp = xmlReadMemory(http_data, strlen(http_data), "FROMIDP", NULL, 0);
    if (doc_fromsp != NULL) {
        char *mem = NULL;
        int size = 0;
        xmlNode *header_fromidp = NULL;
        xmlNode *relay_state = NULL;
        xmlDocDump(stdout, doc_fromidp);
        header_fromidp = getElement(xmlDocGetRootElement(doc_fromidp), "Header");
        freeChildren(header_fromidp);
        relay_state = getElement(header_fromsp, "RelayState");

        xmlAddChild(header_fromidp, xmlCopyNode(relay_state, 1));
fprintf(stdout, "SENDING TO SP >>>>>>>>>>>>>>>>>>>\n");
        xmlDocDump(stdout, doc_fromidp);
        xmlDocDumpMemory(doc_fromidp, &mem, &size);
        return mem;
    }

fprintf(stderr, "NULL RESPONSE BEING SENT>>>>>>>>>>>>>>>\n");
    return NULL;
}

static OM_uint32
eapGssSmInitAuthenticate(OM_uint32 *minor,
                         gss_cred_id_t cred GSSEAP_UNUSED,
                         gss_ctx_id_t ctx,
                         gss_name_t target GSSEAP_UNUSED,
                         gss_OID mech GSSEAP_UNUSED,
                         OM_uint32 reqFlags GSSEAP_UNUSED,
                         OM_uint32 timeReq GSSEAP_UNUSED,
                         gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                         gss_buffer_t inputToken GSSEAP_UNUSED,
                         gss_buffer_t outputToken,
                         OM_uint32 *smFlags)
{
    OM_uint32 major;
    OM_uint32 tmpMinor;
    struct wpabuf *resp = NULL;
    char *saml_response = NULL;

    *minor = 0;

    GSSEAP_ASSERT(inputToken != GSS_C_NO_BUFFER);

#ifdef MECH_EAP
    major = peerConfigInit(minor, ctx);
    if (GSS_ERROR(major))
        goto cleanup;

    GSSEAP_ASSERT(ctx->initiatorCtx.eap != NULL);
    GSSEAP_ASSERT(ctx->flags & CTX_FLAG_EAP_PORT_ENABLED);

    ctx->flags |= CTX_FLAG_EAP_REQ; /* we have a Request from the acceptor */

    wpabuf_set(&ctx->initiatorCtx.reqData,
               inputToken->value, inputToken->length);

    major = GSS_S_CONTINUE_NEEDED;

    eap_peer_sm_step(ctx->initiatorCtx.eap);
    if (ctx->flags & CTX_FLAG_EAP_RESP) {
        ctx->flags &= ~(CTX_FLAG_EAP_RESP);

        resp = eap_get_eapRespData(ctx->initiatorCtx.eap);
    } else if (ctx->flags & CTX_FLAG_EAP_SUCCESS) {
        major = initReady(minor, ctx, reqFlags);
        if (GSS_ERROR(major))
            goto cleanup;

        ctx->flags &= ~(CTX_FLAG_EAP_SUCCESS);
        major = GSS_S_CONTINUE_NEEDED;
        GSSEAP_SM_TRANSITION_NEXT(ctx);
    } else if (ctx->flags & CTX_FLAG_EAP_FAIL) {
        major = GSS_S_DEFECTIVE_CREDENTIAL;
        *minor = GSSEAP_PEER_AUTH_FAILURE;
    } else {
        major = GSS_S_DEFECTIVE_TOKEN;
        *minor = GSSEAP_PEER_BAD_MESSAGE;
    }

cleanup:
    if (resp != NULL) {
        OM_uint32 tmpMajor;
        gss_buffer_desc respBuf;

        GSSEAP_ASSERT(major == GSS_S_CONTINUE_NEEDED);

        respBuf.length = wpabuf_len(resp);
        respBuf.value = (void *)wpabuf_head(resp);

        tmpMajor = duplicateBuffer(&tmpMinor, &respBuf, outputToken);
        if (GSS_ERROR(tmpMajor)) {
            major = tmpMajor;
            *minor = tmpMinor;
        }

        *smFlags |= SM_FLAG_OUTPUT_TOKEN_CRITICAL;
    }

    wpabuf_set(&ctx->initiatorCtx.reqData, NULL, 0);
    peerConfigFree(&tmpMinor, ctx);
#else
#if 0
    if (!strncmp(inputToken->value, "SAML_AUTHREQUEST", strlen("SAML_AUTHREQUEST")))
    {
         fprintf(stderr, "GSSAPI Initiator: Received SAML_AUTHREQUEST from Acceptor\n");
         major = makeStringBuffer(minor, "SAML_ASSERTION_TO_SP", outputToken);
    }
#else
    saml_response = processSAMLRequest(cred, inputToken);
    major = makeStringBuffer(minor, saml_response?:"SAML_ASSERTION_IS_NULL", outputToken);
#endif

    major = GSS_S_COMPLETE;

    GSSEAP_SM_TRANSITION_NEXT(ctx);
        *smFlags |= SM_FLAG_OUTPUT_TOKEN_CRITICAL;
#endif

    return major;
}

static OM_uint32
eapGssSmInitGssFlags(OM_uint32 *minor,
                     gss_cred_id_t cred GSSEAP_UNUSED,
                     gss_ctx_id_t ctx,
                     gss_name_t target GSSEAP_UNUSED,
                     gss_OID mech GSSEAP_UNUSED,
                     OM_uint32 reqFlags GSSEAP_UNUSED,
                     OM_uint32 timeReq GSSEAP_UNUSED,
                     gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                     gss_buffer_t inputToken GSSEAP_UNUSED,
                     gss_buffer_t outputToken,
                     OM_uint32 *smFlags GSSEAP_UNUSED)
{
    unsigned char wireFlags[4];
    gss_buffer_desc flagsBuf;

    store_uint32_be(ctx->gssFlags & GSSEAP_WIRE_FLAGS_MASK, wireFlags);

    flagsBuf.length = sizeof(wireFlags);
    flagsBuf.value = wireFlags;

    return duplicateBuffer(minor, &flagsBuf, outputToken);
}

static OM_uint32
eapGssSmInitGssChannelBindings(OM_uint32 *minor,
                               gss_cred_id_t cred GSSEAP_UNUSED,
                               gss_ctx_id_t ctx,
                               gss_name_t target GSSEAP_UNUSED,
                               gss_OID mech GSSEAP_UNUSED,
                               OM_uint32 reqFlags GSSEAP_UNUSED,
                               OM_uint32 timeReq GSSEAP_UNUSED,
                               gss_channel_bindings_t chanBindings,
                               gss_buffer_t inputToken GSSEAP_UNUSED,
                               gss_buffer_t outputToken,
                               OM_uint32 *smFlags)
{
    OM_uint32 major;
    gss_buffer_desc buffer = GSS_C_EMPTY_BUFFER;

    if (chanBindings != GSS_C_NO_CHANNEL_BINDINGS)
        buffer = chanBindings->application_data;

    major = gssEapWrap(minor, ctx, TRUE, GSS_C_QOP_DEFAULT,
                       &buffer, NULL, outputToken);
    if (GSS_ERROR(major))
        return major;

    GSSEAP_ASSERT(outputToken->value != NULL);

    *minor = 0;
    *smFlags |= SM_FLAG_OUTPUT_TOKEN_CRITICAL;

    return GSS_S_CONTINUE_NEEDED;
}

static OM_uint32
eapGssSmInitInitiatorMIC(OM_uint32 *minor,
                         gss_cred_id_t cred GSSEAP_UNUSED,
                         gss_ctx_id_t ctx,
                         gss_name_t target GSSEAP_UNUSED,
                         gss_OID mech GSSEAP_UNUSED,
                         OM_uint32 reqFlags GSSEAP_UNUSED,
                         OM_uint32 timeReq GSSEAP_UNUSED,
                         gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                         gss_buffer_t inputToken GSSEAP_UNUSED,
                         gss_buffer_t outputToken,
                         OM_uint32 *smFlags)
{
    OM_uint32 major;

    major = gssEapMakeTokenMIC(minor, ctx, outputToken);
    if (GSS_ERROR(major))
        return major;

    GSSEAP_SM_TRANSITION_NEXT(ctx);

    *minor = 0;
    *smFlags |= SM_FLAG_OUTPUT_TOKEN_CRITICAL;

    return GSS_S_CONTINUE_NEEDED;
}
 
#ifdef GSSEAP_ENABLE_REAUTH
static OM_uint32
eapGssSmInitReauthCreds(OM_uint32 *minor,
                        gss_cred_id_t cred,
                        gss_ctx_id_t ctx,
                        gss_name_t target GSSEAP_UNUSED,
                        gss_OID mech GSSEAP_UNUSED,
                        OM_uint32 reqFlags GSSEAP_UNUSED,
                        OM_uint32 timeReq GSSEAP_UNUSED,
                        gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                        gss_buffer_t inputToken,
                        gss_buffer_t outputToken GSSEAP_UNUSED,
                        OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major;

    if (ctx->gssFlags & GSS_C_MUTUAL_FLAG) {
        major = gssEapStoreReauthCreds(minor, ctx, cred, inputToken);
        if (GSS_ERROR(major))
            return major;
    }

    *minor = 0;
    return GSS_S_CONTINUE_NEEDED;
}
#endif /* GSSEAP_ENABLE_REAUTH */

static OM_uint32
eapGssSmInitAcceptorMIC(OM_uint32 *minor,
                        gss_cred_id_t cred GSSEAP_UNUSED,
                        gss_ctx_id_t ctx,
                        gss_name_t target GSSEAP_UNUSED,
                        gss_OID mech GSSEAP_UNUSED,
                        OM_uint32 reqFlags GSSEAP_UNUSED,
                        OM_uint32 timeReq GSSEAP_UNUSED,
                        gss_channel_bindings_t chanBindings GSSEAP_UNUSED,
                        gss_buffer_t inputToken,
                        gss_buffer_t outputToken GSSEAP_UNUSED,
                        OM_uint32 *smFlags GSSEAP_UNUSED)
{
    OM_uint32 major;

    major = gssEapVerifyTokenMIC(minor, ctx, inputToken);
    if (GSS_ERROR(major))
        return major;

    GSSEAP_SM_TRANSITION(ctx, GSSEAP_STATE_ESTABLISHED);

    *minor = 0;

    return GSS_S_COMPLETE;
}

static struct gss_eap_sm eapGssInitiatorSm[] = {
    {
        ITOK_TYPE_CONTEXT_ERR,
        ITOK_TYPE_NONE,
        GSSEAP_STATE_ALL & ~(GSSEAP_STATE_INITIAL),
        0,
        eapGssSmInitError
    },
#ifdef MECH_EAP
    {
        ITOK_TYPE_ACCEPTOR_NAME_RESP,
        ITOK_TYPE_ACCEPTOR_NAME_REQ,
        GSSEAP_STATE_INITIAL | GSSEAP_STATE_AUTHENTICATE,
        0,
        eapGssSmInitAcceptorName
    },
#endif
#ifdef GSSEAP_DEBUG
    {
        ITOK_TYPE_NONE,
        ITOK_TYPE_VENDOR_INFO,
        GSSEAP_STATE_INITIAL,
        0,
        eapGssSmInitVendorInfo
    },
#endif
#ifdef GSSEAP_ENABLE_REAUTH
    {
        ITOK_TYPE_REAUTH_RESP,
        ITOK_TYPE_REAUTH_REQ,
        GSSEAP_STATE_INITIAL | GSSEAP_STATE_REAUTHENTICATE,
        0,
        eapGssSmInitGssReauth
    },
#endif
    {
        ITOK_TYPE_NONE,
#if 1 /* def MECH_EAP */
        ITOK_TYPE_NONE,
#else
        ITOK_TYPE_EAP_REQ,
#endif
#ifdef GSSEAP_ENABLE_REAUTH
        GSSEAP_STATE_REAUTHENTICATE |
#endif
        GSSEAP_STATE_INITIAL,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmInitIdentity
    },
    {
        ITOK_TYPE_EAP_REQ,
        ITOK_TYPE_EAP_RESP,
        GSSEAP_STATE_AUTHENTICATE,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmInitAuthenticate
    },
#ifdef MECH_EAP
    {
        ITOK_TYPE_NONE,
        ITOK_TYPE_GSS_FLAGS,
        GSSEAP_STATE_INITIATOR_EXTS,
        0,
        eapGssSmInitGssFlags
    },
    {
        ITOK_TYPE_NONE,
        ITOK_TYPE_GSS_CHANNEL_BINDINGS,
        GSSEAP_STATE_INITIATOR_EXTS,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmInitGssChannelBindings
    },
    {
        ITOK_TYPE_NONE,
        ITOK_TYPE_INITIATOR_MIC,
        GSSEAP_STATE_INITIATOR_EXTS,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmInitInitiatorMIC
    },
#ifdef GSSEAP_ENABLE_REAUTH
    {
        ITOK_TYPE_REAUTH_CREDS,
        ITOK_TYPE_NONE,
        GSSEAP_STATE_ACCEPTOR_EXTS,
        0,
        eapGssSmInitReauthCreds
    },
#endif
    /* other extensions go here */
    {
        ITOK_TYPE_ACCEPTOR_MIC,
        ITOK_TYPE_NONE,
        GSSEAP_STATE_ACCEPTOR_EXTS,
        SM_ITOK_FLAG_REQUIRED,
        eapGssSmInitAcceptorMIC
    }
#endif
};

OM_uint32
gssEapInitSecContext(OM_uint32 *minor,
                     gss_cred_id_t cred,
                     gss_ctx_id_t ctx,
                     gss_name_t target_name,
                     gss_OID mech_type,
                     OM_uint32 req_flags,
                     OM_uint32 time_req,
                     gss_channel_bindings_t input_chan_bindings,
                     gss_buffer_t input_token,
                     gss_OID *actual_mech_type,
                     gss_buffer_t output_token,
                     OM_uint32 *ret_flags,
                     OM_uint32 *time_rec)
{
    OM_uint32 major, tmpMinor;
    int initialContextToken = (ctx->mechanismUsed == GSS_C_NO_OID);

    /*
     * XXX is acquiring the credential lock here necessary? The password is
     * mutable but the contract could specify that this is not updated whilst
     * a context is being initialized.
     */
    if (cred != GSS_C_NO_CREDENTIAL)
        GSSEAP_MUTEX_LOCK(&cred->mutex);

    if (ctx->cred == GSS_C_NO_CREDENTIAL) {
        major = gssEapResolveInitiatorCred(minor, cred, target_name, &ctx->cred);
        if (GSS_ERROR(major))
            goto cleanup;

        GSSEAP_ASSERT(ctx->cred != GSS_C_NO_CREDENTIAL);
    }

    GSSEAP_MUTEX_LOCK(&ctx->cred->mutex);

    GSSEAP_ASSERT(ctx->cred->flags & CRED_FLAG_RESOLVED);
    GSSEAP_ASSERT(ctx->cred->flags & CRED_FLAG_INITIATE);

    if (initialContextToken) {
        major = initBegin(minor, ctx, target_name, mech_type,
                          req_flags, time_req, input_chan_bindings);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    major = gssEapSmStep(minor,
                         cred,
                         ctx,
                         target_name,
                         mech_type,
                         req_flags,
                         time_req,
                         input_chan_bindings,
                         input_token,
                         output_token,
                         eapGssInitiatorSm,
                         sizeof(eapGssInitiatorSm) / sizeof(eapGssInitiatorSm[0]));
    if (GSS_ERROR(major))
        goto cleanup;

    if (actual_mech_type != NULL) {
        OM_uint32 tmpMajor;

        tmpMajor = gssEapCanonicalizeOid(&tmpMinor, ctx->mechanismUsed, 0, actual_mech_type);
        if (GSS_ERROR(tmpMajor)) {
            major = tmpMajor;
            *minor = tmpMinor;
            goto cleanup;
        }
    }
    if (ret_flags != NULL)
        *ret_flags = ctx->gssFlags;
    if (time_rec != NULL)
        gssEapContextTime(&tmpMinor, ctx, time_rec);

    GSSEAP_ASSERT(CTX_IS_ESTABLISHED(ctx) || major == GSS_S_CONTINUE_NEEDED);

cleanup:
    if (cred != GSS_C_NO_CREDENTIAL)
        GSSEAP_MUTEX_UNLOCK(&cred->mutex);
    if (ctx->cred != GSS_C_NO_CREDENTIAL)
        GSSEAP_MUTEX_UNLOCK(&ctx->cred->mutex);

    return major;
}

OM_uint32 GSSAPI_CALLCONV
gss_init_sec_context(OM_uint32 *minor,
                     gss_cred_id_t cred,
                     gss_ctx_id_t *context_handle,
                     gss_name_t target_name,
                     gss_OID mech_type,
                     OM_uint32 req_flags,
                     OM_uint32 time_req,
                     gss_channel_bindings_t input_chan_bindings,
                     gss_buffer_t input_token,
                     gss_OID *actual_mech_type,
                     gss_buffer_t output_token,
                     OM_uint32 *ret_flags,
                     OM_uint32 *time_rec)
{
    OM_uint32 major, tmpMinor;
    gss_ctx_id_t ctx = *context_handle;

    *minor = 0;

    output_token->length = 0;
    output_token->value = NULL;

    if (ctx == GSS_C_NO_CONTEXT) {
        if (input_token != GSS_C_NO_BUFFER && input_token->length != 0) {
            *minor = GSSEAP_WRONG_SIZE;
            return GSS_S_DEFECTIVE_TOKEN;
        }

        major = gssEapAllocContext(minor, &ctx);
        if (GSS_ERROR(major))
            return major;

        ctx->flags |= CTX_FLAG_INITIATOR;

        *context_handle = ctx;
    }

    GSSEAP_MUTEX_LOCK(&ctx->mutex);

#if 1 /* def MECH_EAP */
    major = gssEapInitSecContext(minor,
                                 cred,
                                 ctx,
                                 target_name,
                                 mech_type,
                                 req_flags,
                                 time_req,
                                 input_chan_bindings,
                                 input_token,
                                 actual_mech_type,
                                 output_token,
                                 ret_flags,
                                 time_rec);
#else
/* VSY: Cred delegation not supported in SAML EC, so GSS_C_DELEG_FLAG should be FALSE
 * VSY: Per kitten, GSS_C_MUTUAL_FLAG is always set but it also says:
 * "The mutual authentication property of this mechanism relies on
 *  successfully comparing the TLS server identity with the negotiated
 *  target name.  Since the TLS channel is managed by the application
 *  outside of the GSS-API mechanism, the mechanism itself is unable to
 *  confirm the name while the application is able to perform this
 *  comparison for the mechanism.  For this reason, applications MUST
 *  match the TLS server identity with the target name, as discussed in
 *  [RFC6125]."
 *  
 *  Refer to "5.19. gss_init_sec_context" in RFC 2744 for performing the below.
 *
 *  if (input_token is null or empty)
 *      generate output token as "OID+n,,", specifically
 *      as "1.1.3.6.1.4.1.11591.4.6n,," in output token base64 encoded
 *      return GSS_S_CONTINUE_NEEDED as major status
 *  else
 *      verify the input_token as a SOAP AuthnRequest
 *      retain a copy of Relay State for later use
 *      communicate SOAP body + basic auth header to the configured IdP
 *      wait for reply back from IdP
 *      In the SOAP reply received, replace relay state with one saved earlier
 *      Prepare an output token with the previously altered SOAP reply
 *      return GSS_S_COMPLETE as major status
 *  else
 *      return GSS_S_DEFECTIVE_TOKEN
 *
*/
        if (input_token == GSS_C_NO_BUFFER || input_token->length == 0) {
            /* TODO really should check for first invocation of this here */
            /* TODO handle output_token == GSS_C_NO_BUFFER */
            output_token->value = strdup("1.1.3.6.1.4.1.11591.4.6n,,");
            output_token->length = strlen("1.1.3.6.1.4.1.11591.4.6n,,")+1;
            major = GSS_S_CONTINUE_NEEDED;
        } else if (!strcmp((char*)input_token->value, "SAML_AUTHREQUEST")) {
           gss_buffer_desc name_buf;
            /* TODO obtain SAML assertion fro IdP */
            output_token->value = strdup("SAML_ASSERTION_TO_SP");
            output_token->length = strlen("SAML_ASSERTION_TO_SP")+1;

           name_buf.value = "imaclient";
           name_buf.length = strlen(name_buf.value) + 1;
            major = gssEapImportName(&tmpMinor, &name_buf, GSS_C_NT_USER_NAME,
                              GSS_C_NO_OID, &(ctx->initiatorName));

           name_buf.value = "imaserver";
           name_buf.length = strlen(name_buf.value) + 1;
            major = gssEapImportName(&tmpMinor, &name_buf, GSS_C_NT_USER_NAME,
                              GSS_C_NO_OID, &(ctx->acceptorName));
    major = gssEapCanonicalizeOid(minor,
                                  GSS_SAMLEC_MECHANISM,
                                  OID_FLAG_FAMILY_MECH_VALID,
                                  &ctx->mechanismUsed);
            major=GSS_S_COMPLETE;

        } else {
            major = GSS_S_DEFECTIVE_TOKEN;
        }
#endif

    GSSEAP_MUTEX_UNLOCK(&ctx->mutex);

    if (GSS_ERROR(major))
        gssEapReleaseContext(&tmpMinor, context_handle);
#if 0 /* TODO: Fix this: ignore this error from the State Machine for now */
#endif

    return major;
}
