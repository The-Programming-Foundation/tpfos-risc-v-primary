/*-
 * Copyright (c) 1997 Nicolas Souchu
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$Id: ppb_1284.h,v 1.3 1998/09/13 18:26:26 nsouch Exp $
 *
 */
#ifndef __1284_H
#define __1284_H

/*
 * IEEE1284 signals
 */

/* host driven signals */

#define nHostClk	STROBE
#define Write		STROBE

#define nHostBusy	AUTOFEED
#define nHostAck	AUTOFEED
#define DStrb		AUTOFEED

#define nReveseRequest	nINIT

#define nActive1284	SELECTIN
#define AStrb		SELECTIN

/* peripheral driven signals */

#define nDataAvail	nFAULT
#define nPeriphRequest	nFAULT

#define Xflag		SELECT

#define AckDataReq	PERROR
#define nAckReverse	PERROR

#define nPtrBusy	nBUSY
#define nPeriphAck	nBUSY
#define Wait		nBUSY

#define PtrClk		nACK
#define PeriphClk	nACK
#define Intr		nACK

/* request mode values */
#define NIBBLE_1284_NORMAL	0
#define NIBBLE_1284_REQUEST_ID	4

/* how to terminate */
#define VALID_STATE	0
#define IMMEDIATE	1

extern int do_1284_wait(struct ppb_device *, char, char);

extern int nibble_1284_inbyte(struct ppb_device *, char *);
extern void nibble_1284_sync(struct ppb_device *);
extern int nibble_1284_mode(struct ppb_device *, int);

extern int ppb_1284_negociate(struct ppb_device *, int);
extern int ppb_1284_terminate(struct ppb_device *, int how);

#endif
