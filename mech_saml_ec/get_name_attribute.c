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

#include "gssapiP_eap.h"

/*
 * Wrapper for retrieving a naming attribute.
 */

#ifndef MECH_EAP
int getSAMLAttribute(const char* attrib, char** value);
#endif

OM_uint32 GSSAPI_CALLCONV
gss_get_name_attribute(OM_uint32 *minor,
                       gss_name_t name,
                       gss_buffer_t attr,
                       int *authenticated,
                       int *complete,
                       gss_buffer_t value,
                       gss_buffer_t display_value,
                       int *more)
{
    OM_uint32 major;

    *minor = 0;

    if (name == GSS_C_NO_NAME) {
        *minor = EINVAL;
        return GSS_S_CALL_INACCESSIBLE_READ | GSS_S_BAD_NAME;
    }

#ifdef MECH_EAP
    GSSEAP_MUTEX_LOCK(&name->mutex);

    major = gssEapGetNameAttribute(minor, name, attr,
                                   authenticated, complete,
                                   value, display_value, more);

    GSSEAP_MUTEX_UNLOCK(&name->mutex);
#else
    char *attr_str = NULL;
    major = bufferToString(minor, attr, &attr_str);
    if (major == GSS_S_COMPLETE) {
        if (getSAMLAttribute(attr_str, &value->value) == 1) {
            if (MECH_SAML_EC_DEBUG)
                fprintf(stdout, "gss_get_name_attribute():"
                            " attribute (%s) has value (%s)\n",
                            attr_str, value->value);
            value->length = strlen(value->value);
            if (authenticated != NULL)
                *authenticated = 1;
            if (complete != NULL)
                *complete = 1;
            if (more != NULL)
                *more = 0;
            major = duplicateBuffer(minor, value, display_value);
        } else
            major = GSS_S_UNAVAILABLE;
        free(attr_str); attr_str = NULL;
    }
#endif

    return major;
}
