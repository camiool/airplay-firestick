/*
 * Minimal replacement for Apple's <dns_sd.h> used by UxPlay's dnssd.c.
 *
 * Android has no Bonjour/Avahi daemon reachable from native code, so the
 * actual service registration is forwarded (through a hook installed by the
 * JNI bridge) to android.net.nsd.NsdManager on the Java side.
 *
 * Only the subset of the API that dnssd.c uses is implemented.
 */
#ifndef AIRPLAY_FIRESTICK_DNS_SD_H
#define AIRPLAY_FIRESTICK_DNS_SD_H

#include <stdint.h>
#include <arpa/inet.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _DNSServiceRef_t *DNSServiceRef;
typedef uint32_t DNSServiceFlags;
typedef int32_t DNSServiceErrorType;

typedef union _TXTRecordRef_t {
    char PrivateData[16];
    char *ForceNaturalAlignment;
} TXTRecordRef;

enum {
    kDNSServiceErr_NoError = 0,
    kDNSServiceErr_Unknown = -65537,
    kDNSServiceErr_NoMemory = -65539,
    kDNSServiceErr_BadParam = -65540,
    kDNSServiceErr_ServiceNotRunning = -65563
};

typedef void (*DNSServiceRegisterReply)(DNSServiceRef sdRef, DNSServiceFlags flags,
                                        DNSServiceErrorType errorCode, const char *name,
                                        const char *regtype, const char *domain, void *context);

DNSServiceErrorType DNSServiceRegister(DNSServiceRef *sdRef, DNSServiceFlags flags,
                                       uint32_t interfaceIndex, const char *name,
                                       const char *regtype, const char *domain,
                                       const char *host, uint16_t port, uint16_t txtLen,
                                       const void *txtRecord, DNSServiceRegisterReply callBack,
                                       void *context);
void DNSServiceRefDeallocate(DNSServiceRef sdRef);

void TXTRecordCreate(TXTRecordRef *txtRecord, uint16_t bufferLen, void *buffer);
void TXTRecordDeallocate(TXTRecordRef *txtRecord);
DNSServiceErrorType TXTRecordSetValue(TXTRecordRef *txtRecord, const char *key,
                                      uint8_t valueSize, const void *value);
uint16_t TXTRecordGetLength(const TXTRecordRef *txtRecord);
const void *TXTRecordGetBytesPtr(const TXTRecordRef *txtRecord);

/* ---- Android hooks (implemented by the JNI bridge) ---------------------- */

/* Register a service. `port` is in host byte order. `txt` is a standard DNS
 * TXT rdata blob (sequence of <len><key=value>). Returns an opaque non-zero
 * handle on success, 0 on failure. */
typedef intptr_t (*dnssd_shim_register_fn)(const char *name, const char *regtype,
                                           uint16_t port, const uint8_t *txt,
                                           uint16_t txt_len);
typedef void (*dnssd_shim_unregister_fn)(intptr_t handle);

void dnssd_shim_set_hooks(dnssd_shim_register_fn reg, dnssd_shim_unregister_fn unreg);

#ifdef __cplusplus
}
#endif
#endif
