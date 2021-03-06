/*
 * netboot - trivial UEFI first-stage bootloader netboot support
 *
 * Copyright 2012 Red Hat, Inc <mjg@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Significant portions of this code are derived from Tianocore
 * (http://tianocore.sf.net) and are Copyright 2009-2012 Intel
 * Corporation.
 */

#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "shim.h"
#include "netboot.h"

static inline unsigned short int __swap16(unsigned short int x)
{
        __asm__("xchgb %b0,%h0"
                : "=q" (x)
                : "0" (x));
	return x;
}

#define ntohs(x) __swap16(x)
#define htons(x) ntohs(x)

static EFI_PXE_BASE_CODE *pxe;
static EFI_IP_ADDRESS tftp_addr;
static CHAR8 *full_path;


typedef struct {
	UINT16 OpCode;
	UINT16 Length;
	UINT8 Data[1];
} EFI_DHCP6_PACKET_OPTION;

static CHAR8 *
translate_slashes(char *str)
{
	int i;
	int j;
	if (str == NULL)
		return (CHAR8 *)str;

	for (i = 0, j = 0; str[i] != '\0'; i++, j++) {
		if (str[i] == '\\') {
			str[j] = '/';
			if (str[i+1] == '\\')
				i++;
		}
	}
	return (CHAR8 *)str;
}

/*
 * usingNetboot
 * Returns TRUE if we identify a protocol that is enabled and Providing us with
 * the needed information to fetch a grubx64.efi image
 */
BOOLEAN findNetboot(EFI_HANDLE image_handle)
{
	UINTN bs = sizeof(EFI_HANDLE);
	EFI_GUID pxe_base_code_protocol = EFI_PXE_BASE_CODE_PROTOCOL;
	EFI_HANDLE *hbuf;
	BOOLEAN rc = FALSE;
	void *buffer = AllocatePool(bs);
	UINTN errcnt = 0;
	UINTN i;
	EFI_STATUS status;

	if (!buffer)
		return FALSE;

try_again:
	status = uefi_call_wrapper(BS->LocateHandle,5, ByProtocol, 
				   &pxe_base_code_protocol, NULL, &bs,
				   buffer);

	if (status == EFI_BUFFER_TOO_SMALL) {
		errcnt++;
		FreePool(buffer);
		if (errcnt > 1)
			return FALSE;
		buffer = AllocatePool(bs);
		if (!buffer)
			return FALSE;
		goto try_again;
	}

	if (status == EFI_NOT_FOUND) {
		FreePool(buffer);
		return FALSE;
	}

	/*
 	 * We have a list of pxe supporting protocols, lets see if any are
 	 * active
 	 */
	hbuf = buffer;
	pxe = NULL;
	for (i=0; i < (bs / sizeof(EFI_HANDLE)); i++) {
		status = uefi_call_wrapper(BS->OpenProtocol, 6, hbuf[i],
					   &pxe_base_code_protocol,
					   (void **)&pxe, image_handle, NULL,
					   EFI_OPEN_PROTOCOL_GET_PROTOCOL);

		if (status != EFI_SUCCESS) {
			pxe = NULL;
			continue;
		}

		if (!pxe || !pxe->Mode) {
			pxe = NULL;
			continue;
		}

		if (pxe->Mode->Started && pxe->Mode->DhcpAckReceived) {
			/*
 			 * We've located a pxe protocol handle thats been 
 			 * started and has received an ACK, meaning its
 			 * something we'll be able to get tftp server info
 			 * out of
 			 */
			rc = TRUE;
			break;
		}
			
	}

	FreePool(buffer);
	return rc;
}

static CHAR8 *get_v6_bootfile_url(EFI_PXE_BASE_CODE_DHCPV6_PACKET *pkt)
{
	void *optr;
	EFI_DHCP6_PACKET_OPTION *option;
	CHAR8 *url;
	UINT32 urllen;

	optr = pkt->DhcpOptions;

	for(;;) {
		option = (EFI_DHCP6_PACKET_OPTION *)optr;

		if (ntohs(option->OpCode) == 0)
			return NULL;

		if (ntohs(option->OpCode) == 59) {
			/* This is the bootfile url option */
			urllen = ntohs(option->Length);
			url = AllocateZeroPool(urllen+1);
			if (!url)
				return NULL;
			memcpy(url, option->Data, urllen);
			return url;
		}
		optr += 4 + ntohs(option->Length);
	}

	return NULL;
}

static CHAR16 str2ns(CHAR8 *str)
{
        CHAR16 ret = 0;
        CHAR8 v;
        for(;*str;str++) {
                if ('0' <= *str && *str <= '9')
                        v = *str - '0';
                else if ('A' <= *str && *str <= 'F')
                        v = *str - 'A' + 10;
                else if ('a' <= *str && *str <= 'f')
                        v = *str - 'a' + 10;
                else
                        v = 0;
                ret = (ret << 4) + v;
        }
        return htons(ret);
}

static CHAR8 *str2ip6(CHAR8 *str)
{
        UINT8 i, j, p;
	size_t len;
        CHAR8 *a, *b, t;
        static UINT16 ip[8];

        for(i=0; i < 8; i++) {
                ip[i] = 0;
        }
        len = strlen(str);
        a = b = str;
        for(i=p=0; i < len; i++, b++) {
                if (*b != ':')
                        continue;
                *b = '\0';
                ip[p++] = str2ns(a);
                *b = ':';
                a = b + 1;
                if ( *(b+1) == ':' )
                        break;
        }
        a = b = (str + len);
        for(j=len, p=7; j > i; j--, a--) {
                if (*a != ':')
                        continue;
                t = *b;
                *b = '\0';
                ip[p--] = str2ns(a+1);
                *b = t;
                b = a;
        }
        return (CHAR8 *)ip;
}

static BOOLEAN extract_tftp_info(CHAR8 *url)
{
	CHAR8 *start, *end;
	CHAR8 ip6str[40];
	CHAR8 *template = (CHAR8 *)translate_slashes(DEFAULT_LOADER_CHAR);

	if (strncmp((UINT8 *)url, (UINT8 *)"tftp://", 7)) {
		Print(L"URLS MUST START WITH tftp://\n");
		return FALSE;
	}
	start = url + 7;
	if (*start != '[') {
		Print(L"TFTP SERVER MUST BE ENCLOSED IN [..]\n");
		return FALSE;
	}

	start++;
	end = start;
	while ((*end != '\0') && (*end != ']')) {
		end++;
		if (end - start > 39) {
			Print(L"TFTP URL includes malformed IPv6 address\n");
			return FALSE;
		}
	}
	if (end == '\0') {
		Print(L"TFTP SERVER MUST BE ENCLOSED IN [..]\n");
		return FALSE;
	}
	memset(ip6str, 0, 40);
	memcpy(ip6str, start, end - start);
	end++;
	memcpy(&tftp_addr.v6, str2ip6(ip6str), 16);
	full_path = AllocateZeroPool(strlen(end)+strlen(template)+1);
	if (!full_path)
		return FALSE;
	memcpy(full_path, end, strlen(end));
	end = (CHAR8 *)strrchr((char *)full_path, '/');
	if (!end)
		end = (CHAR8 *)full_path;
	memcpy(end, template, strlen(template));
	end[strlen(template)] = '\0';

	return TRUE;
}

static EFI_STATUS parseDhcp6()
{
	EFI_PXE_BASE_CODE_DHCPV6_PACKET *packet = (EFI_PXE_BASE_CODE_DHCPV6_PACKET *)&pxe->Mode->DhcpAck.Raw;
	CHAR8 *bootfile_url;

	bootfile_url = get_v6_bootfile_url(packet);
	if (!bootfile_url)
		return EFI_NOT_FOUND;
	if (extract_tftp_info(bootfile_url) == FALSE) {
		FreePool(bootfile_url);
		return EFI_NOT_FOUND;
	}
	FreePool(bootfile_url);
	return EFI_SUCCESS;
}

static EFI_STATUS parseDhcp4()
{
	CHAR8 *template = (CHAR8 *)DEFAULT_LOADER_CHAR;
	full_path = AllocateZeroPool(strlen(template)+1);

	if (!full_path)
		return EFI_OUT_OF_RESOURCES;

	memcpy(&tftp_addr.v4, pxe->Mode->DhcpAck.Dhcpv4.BootpSiAddr, 4);

	memcpy(full_path, template, strlen(template));

	/* Note we don't capture the filename option here because we know its shim.efi
	 * We instead assume the filename at the end of the path is going to be grubx64.efi
	 */
	return EFI_SUCCESS;
}

EFI_STATUS parseNetbootinfo(EFI_HANDLE image_handle)
{

	EFI_STATUS rc;

	if (!pxe)
		return EFI_NOT_READY;

	memset((UINT8 *)&tftp_addr, 0, sizeof(tftp_addr));

	/*
	 * If we've discovered an active pxe protocol figure out
	 * if its ipv4 or ipv6
	 */
	if (pxe->Mode->UsingIpv6){
		rc = parseDhcp6();
	} else
		rc = parseDhcp4();
	return rc;
}

EFI_STATUS FetchNetbootimage(EFI_HANDLE image_handle, VOID **buffer, UINT64 *bufsiz)
{
	EFI_STATUS rc;
	EFI_PXE_BASE_CODE_TFTP_OPCODE read = EFI_PXE_BASE_CODE_TFTP_READ_FILE;
	BOOLEAN overwrite = FALSE;
	BOOLEAN nobuffer = FALSE;
	UINTN blksz = 512;

	Print(L"Fetching Netboot Image\n");
	if (*buffer == NULL) {
		*buffer = AllocatePool(4096 * 1024);
		if (!*buffer)
			return EFI_OUT_OF_RESOURCES; 
		*bufsiz = 4096 * 1024;
	}

try_again:
	rc = uefi_call_wrapper(pxe->Mtftp, 10, pxe, read, *buffer, overwrite,
				bufsiz, &blksz, &tftp_addr, full_path, NULL, nobuffer);

	if (rc == EFI_BUFFER_TOO_SMALL) {
		/* try again, doubling buf size */
		*bufsiz *= 2;
		FreePool(*buffer);
		*buffer = AllocatePool(*bufsiz);
		if (!*buffer)
			return EFI_OUT_OF_RESOURCES;
		goto try_again;
	}

	if (rc != EFI_SUCCESS && *buffer) {
		FreePool(*buffer);
	}
	return rc;
}
