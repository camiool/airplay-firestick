/* See dns_sd.h for an explanation of this shim. */
#include "dns_sd.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    uint16_t len;
    uint16_t cap;
} txt_impl_t;

struct _DNSServiceRef_t {
    intptr_t handle;
};

static dnssd_shim_register_fn g_register = NULL;
static dnssd_shim_unregister_fn g_unregister = NULL;

void dnssd_shim_set_hooks(dnssd_shim_register_fn reg, dnssd_shim_unregister_fn unreg) {
    g_register = reg;
    g_unregister = unreg;
}

static txt_impl_t *txt_of(const TXTRecordRef *ref) {
    txt_impl_t *impl;
    memcpy(&impl, ref->PrivateData, sizeof(impl));
    return impl;
}

void TXTRecordCreate(TXTRecordRef *txtRecord, uint16_t bufferLen, void *buffer) {
    (void) bufferLen;
    (void) buffer;
    txt_impl_t *impl = (txt_impl_t *) calloc(1, sizeof(txt_impl_t));
    memset(txtRecord, 0, sizeof(*txtRecord));
    memcpy(txtRecord->PrivateData, &impl, sizeof(impl));
}

void TXTRecordDeallocate(TXTRecordRef *txtRecord) {
    txt_impl_t *impl = txt_of(txtRecord);
    if (impl) {
        free(impl->buf);
        free(impl);
    }
    memset(txtRecord, 0, sizeof(*txtRecord));
}

/* Remove an existing entry for `key` (TXTRecordSetValue replaces values). */
static void txt_remove_key(txt_impl_t *impl, const char *key) {
    size_t klen = strlen(key);
    uint16_t pos = 0;
    while (pos < impl->len) {
        uint8_t elen = impl->buf[pos];
        const uint8_t *e = impl->buf + pos + 1;
        if (elen >= klen && memcmp(e, key, klen) == 0 && (elen == klen || e[klen] == '=')) {
            uint16_t total = (uint16_t) (elen + 1);
            memmove(impl->buf + pos, impl->buf + pos + total, impl->len - pos - total);
            impl->len -= total;
            continue;
        }
        pos += (uint16_t) (elen + 1);
    }
}

DNSServiceErrorType TXTRecordSetValue(TXTRecordRef *txtRecord, const char *key,
                                      uint8_t valueSize, const void *value) {
    txt_impl_t *impl = txt_of(txtRecord);
    if (!impl || !key) return kDNSServiceErr_BadParam;
    size_t klen = strlen(key);
    size_t elen = klen + (value ? 1 + valueSize : 0);
    if (elen > 255) return kDNSServiceErr_BadParam;
    txt_remove_key(impl, key);
    if ((size_t) impl->len + elen + 1 > 65535) return kDNSServiceErr_NoMemory;
    if (impl->len + elen + 1 > impl->cap) {
        uint16_t ncap = (uint16_t) ((impl->len + elen + 1) * 2 > 65535 ? 65535 : (impl->len + elen + 1) * 2);
        uint8_t *nbuf = (uint8_t *) realloc(impl->buf, ncap);
        if (!nbuf) return kDNSServiceErr_NoMemory;
        impl->buf = nbuf;
        impl->cap = ncap;
    }
    uint8_t *p = impl->buf + impl->len;
    *p++ = (uint8_t) elen;
    memcpy(p, key, klen);
    p += klen;
    if (value) {
        *p++ = '=';
        memcpy(p, value, valueSize);
    }
    impl->len = (uint16_t) (impl->len + elen + 1);
    return kDNSServiceErr_NoError;
}

uint16_t TXTRecordGetLength(const TXTRecordRef *txtRecord) {
    txt_impl_t *impl = txt_of(txtRecord);
    return impl ? impl->len : 0;
}

const void *TXTRecordGetBytesPtr(const TXTRecordRef *txtRecord) {
    txt_impl_t *impl = txt_of(txtRecord);
    return impl ? impl->buf : NULL;
}

DNSServiceErrorType DNSServiceRegister(DNSServiceRef *sdRef, DNSServiceFlags flags,
                                       uint32_t interfaceIndex, const char *name,
                                       const char *regtype, const char *domain,
                                       const char *host, uint16_t port, uint16_t txtLen,
                                       const void *txtRecord, DNSServiceRegisterReply callBack,
                                       void *context) {
    (void) flags; (void) interfaceIndex; (void) domain; (void) host;
    (void) callBack; (void) context;
    if (!sdRef || !name || !regtype) return kDNSServiceErr_BadParam;
    if (!g_register) return kDNSServiceErr_ServiceNotRunning;
    intptr_t handle = g_register(name, regtype, ntohs(port), (const uint8_t *) txtRecord, txtLen);
    if (!handle) return kDNSServiceErr_Unknown;
    struct _DNSServiceRef_t *ref = (struct _DNSServiceRef_t *) calloc(1, sizeof(*ref));
    if (!ref) {
        if (g_unregister) g_unregister(handle);
        return kDNSServiceErr_NoMemory;
    }
    ref->handle = handle;
    *sdRef = ref;
    return kDNSServiceErr_NoError;
}

void DNSServiceRefDeallocate(DNSServiceRef sdRef) {
    if (!sdRef) return;
    if (g_unregister) g_unregister(sdRef->handle);
    free(sdRef);
}
