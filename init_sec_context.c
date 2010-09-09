/*
 * Copyright (c) 2010, JANET(UK)
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

#include "gssapiP_eap.h"

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

static int
peerGetInt(void *data, enum eapol_int_var variable)
{
    gss_ctx_id_t ctx = data;

    if (ctx == GSS_C_NO_CONTEXT)
        return FALSE;

    assert(CTX_IS_INITIATOR(ctx));

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

    assert(CTX_IS_INITIATOR(ctx));

    switch (variable) {
    case EAPOL_idleWhile:
        ctx->initiatorCtx.idleWhile = value;
        break;
    }
}

static OM_uint32
eapGssSmInitAuthenticate(OM_uint32 *minor,
                         gss_cred_id_t cred,
                         gss_ctx_id_t ctx,
                         gss_name_t target,
                         gss_OID mech,
                         OM_uint32 reqFlags,
                         OM_uint32 timeReq,
                         gss_channel_bindings_t chanBindings,
                         gss_buffer_t inputToken,
                         gss_buffer_t outputToken)
{
    OM_uint32 major, tmpMinor;
    time_t now;

    if (inputToken == GSS_C_NO_BUFFER || inputToken->length == 0) {
        /* first time */
        reqFlags &= GSS_C_TRANS_FLAG | GSS_C_REPLAY_FLAG | GSS_C_DCE_STYLE;
        ctx->gssFlags |= reqFlags;

        time(&now);

        if (timeReq == 0 || timeReq == GSS_C_INDEFINITE)
            ctx->expiryTime = 0;
        else
            ctx->expiryTime = now + timeReq;

        major = gss_duplicate_name(minor, cred->name, &ctx->initiatorName);
        if (GSS_ERROR(major))
            goto cleanup;

        major = gss_duplicate_name(minor, target, &ctx->acceptorName);
        if (GSS_ERROR(major))
            goto cleanup;

        if (mech == GSS_C_NULL_OID || oidEqual(mech, GSS_EAP_MECHANISM)) {
            major = gssEapDefaultMech(minor, &ctx->mechanismUsed);
        } else if (gssEapIsConcreteMechanismOid(mech)) {
            if (!gssEapInternalizeOid(mech, &ctx->mechanismUsed))
                major = duplicateOid(minor, mech, &ctx->mechanismUsed);
        } else {
            major = GSS_S_BAD_MECH;
        }
        if (GSS_ERROR(major))
            goto cleanup;
    }

cleanup:
    return major;
}

static struct eap_gss_initiator_sm {
    enum gss_eap_token_type inputTokenType;
    enum gss_eap_token_type outputTokenType;
    OM_uint32 (*processToken)(OM_uint32 *,
                              gss_cred_id_t,
                              gss_ctx_id_t,
                              gss_name_t,
                              gss_OID,
                              OM_uint32,
                              OM_uint32,
                              gss_channel_bindings_t,
                              gss_buffer_t,
                              gss_buffer_t);
} eapGssInitiatorSm[] = {
    { TOK_TYPE_EAP_REQ, TOK_TYPE_EAP_RESP, eapGssSmInitAuthenticate },
};

OM_uint32
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
    struct eap_gss_initiator_sm *sm = NULL;
    gss_buffer_desc innerInputToken, innerOutputToken;

    *minor = 0;

    innerOutputToken.length = 0;
    innerOutputToken.value = NULL;

    output_token->length = 0;
    output_token->value = NULL;

    if (cred != GSS_C_NO_CREDENTIAL && !(cred->flags & CRED_FLAG_INITIATE)) {
        return GSS_S_NO_CRED;
    }

    if (ctx == GSS_C_NO_CONTEXT) {
        if (input_token != GSS_C_NO_BUFFER && input_token->length != 0) {
            return GSS_S_DEFECTIVE_TOKEN;
        }

        major = gssEapAllocContext(minor, &ctx);
        if (GSS_ERROR(major))
            return major;

        ctx->flags |= CTX_FLAG_INITIATOR;

        *context_handle = ctx;
    }

    GSSEAP_MUTEX_LOCK(&ctx->mutex);

    sm = &eapGssInitiatorSm[ctx->state];

    if (input_token != GSS_C_NO_BUFFER) {
        major = gssEapVerifyToken(minor, ctx, input_token,
                                  sm->inputTokenType, &innerInputToken);
        if (GSS_ERROR(major))
            goto cleanup;
    } else {
        innerInputToken.length = 0;
        innerInputToken.value = NULL;
    }

    major = (sm->processToken)(minor,
                               cred,
                               ctx,
                               target_name,
                               mech_type,
                               req_flags,
                               time_req,
                               input_chan_bindings,
                               &innerInputToken,
                               &innerOutputToken);
    if (GSS_ERROR(major))
        goto cleanup;

    if (actual_mech_type != NULL) {
        if (!gssEapInternalizeOid(ctx->mechanismUsed, actual_mech_type))
            duplicateOid(&tmpMinor, ctx->mechanismUsed, actual_mech_type);
    }
    if (innerOutputToken.length != 0) {
        major = gssEapMakeToken(minor, ctx, &innerOutputToken,
                                sm->outputTokenType, output_token);
        if (GSS_ERROR(major))
            goto cleanup;
    }
    if (ret_flags != NULL)
        *ret_flags = ctx->gssFlags;
    if (time_rec != NULL)
        gss_context_time(&tmpMinor, ctx, time_rec);

    assert(ctx->state == EAP_STATE_ESTABLISHED || major == GSS_S_CONTINUE_NEEDED);

cleanup:
    GSSEAP_MUTEX_UNLOCK(&ctx->mutex);

    if (GSS_ERROR(major))
        gssEapReleaseContext(&tmpMinor, context_handle);

    gss_release_buffer(&tmpMinor, &innerOutputToken);

    return major;
}
