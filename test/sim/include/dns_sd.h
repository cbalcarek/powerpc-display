#include <stdint.h>
typedef struct _DNSServiceRef_t *DNSServiceRef; typedef unsigned int DNSServiceFlags; typedef int DNSServiceErrorType; enum { kDNSServiceErr_NoError=0 };
typedef void (*DNSServiceRegisterReply)(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType, const char*, const char*, const char*, void*);
DNSServiceErrorType DNSServiceRegister(DNSServiceRef*, DNSServiceFlags, unsigned, const char*, const char*, const char*, const char*, unsigned short, unsigned short, const void*, DNSServiceRegisterReply, void*);
void DNSServiceRefDeallocate(DNSServiceRef); int DNSServiceRefSockFD(DNSServiceRef); DNSServiceErrorType DNSServiceProcessResult(DNSServiceRef);
typedef void (*DNSServiceResolveReply)(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType, const char*, const char*, uint16_t, uint16_t, const unsigned char*, void*);
DNSServiceErrorType DNSServiceResolve(DNSServiceRef*, DNSServiceFlags, uint32_t, const char*, const char*, const char*, DNSServiceResolveReply, void*);
