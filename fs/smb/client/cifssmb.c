// SPDX-License-Identifier: LGPL-2.1
/*
 *
 *   Copyright (C) International Business Machines  Corp., 2002,2010
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *
 *   Contains the routines for constructing the SMB PDUs themselves
 *
 */

 /* SMB/CIFS PDU handling routines here - except for leftovers in connect.c   */
 /* These are mostly routines that operate on a pathname, or on a tree id     */
 /* (mounted volume), but there are eight handle based routines which must be */
 /* treated slightly differently for reconnection purposes since we never     */
 /* want to reuse a stale file handle and only the caller knows the file info */

#include <linux/fs.h>
#include <linux/filelock.h>
#include <linux/kernel.h>
#include <linux/vfs.h>
#include <linux/slab.h>
#include <linux/posix_acl_xattr.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/uaccess.h>
#include <linux/netfs.h>
#include <trace/events/netfs.h>
#include "cifspdu.h"
#include "cifsfs.h"
#include "cifsglob.h"
#include "cifsacl.h"
#include "cifsproto.h"
#include "cifs_unicode.h"
#include "cifs_debug.h"
#include "fscache.h"
#include "smbdirect.h"
#ifdef CONFIG_CIFS_DFS_UPCALL
#include "dfs_cache.h"
#endif

#ifdef CONFIG_CIFS_POSIX
static struct {
	int index;
	char *name;
} protocols[] = {
	{CIFS_PROT, "\2NT LM 0.12"},
	{POSIX_PROT, "\2POSIX 2"},
	{BAD_PROT, "\2"}
};
#else
static struct {
	int index;
	char *name;
} protocols[] = {
	{CIFS_PROT, "\2NT LM 0.12"},
	{BAD_PROT, "\2"}
};
#endif

/* define the number of elements in the cifs dialect array */
#ifdef CONFIG_CIFS_POSIX
#define CIFS_NUM_PROT 2
#else /* not posix */
#define CIFS_NUM_PROT 1
#endif /* CIFS_POSIX */


/* reconnect the socket, tcon, and smb session if needed */
static int
cifs_reconnect_tcon(struct cifs_tcon *tcon, int smb_command)
{
	struct TCP_Server_Info *server;
	struct cifs_ses *ses;
	int rc;

	/*
	 * SMBs NegProt, SessSetup, uLogoff do not have tcon yet so check for
	 * tcp and smb session status done differently for those three - in the
	 * calling routine
	 */
	if (!tcon)
		return 0;

	ses = tcon->ses;
	server = ses->server;

	/*
	 * only tree disconnect, open, and write, (and ulogoff which does not
	 * have tcon) are allowed as we start umount
	 */
	spin_lock(&tcon->tc_lock);
	if (tcon->status == TID_EXITING) {
		if (smb_command != SMB_COM_TREE_DISCONNECT) {
			spin_unlock(&tcon->tc_lock);
			cifs_dbg(FYI, "can not send cmd %d while umounting\n",
				 smb_command);
			return -ENODEV;
		}
	}
	spin_unlock(&tcon->tc_lock);

again:
	rc = cifs_wait_for_server_reconnect(server, tcon->retry);
	if (rc)
		return rc;

	spin_lock(&ses->chan_lock);
	if (!cifs_chan_needs_reconnect(ses, server) && !tcon->need_reconnect) {
		spin_unlock(&ses->chan_lock);
		return 0;
	}
	spin_unlock(&ses->chan_lock);

	mutex_lock(&ses->session_mutex);
	/*
	 * Handle the case where a concurrent thread failed to negotiate or
	 * killed a channel.
	 */
	spin_lock(&server->srv_lock);
	switch (server->tcpStatus) {
	case CifsExiting:
		spin_unlock(&server->srv_lock);
		mutex_unlock(&ses->session_mutex);
		return -EHOSTDOWN;
	case CifsNeedReconnect:
		spin_unlock(&server->srv_lock);
		mutex_unlock(&ses->session_mutex);
		if (!tcon->retry)
			return -EHOSTDOWN;
		goto again;
	default:
		break;
	}
	spin_unlock(&server->srv_lock);

	/*
	 * need to prevent multiple threads trying to simultaneously
	 * reconnect the same SMB session
	 */
	spin_lock(&ses->ses_lock);
	spin_lock(&ses->chan_lock);
	if (!cifs_chan_needs_reconnect(ses, server) &&
	    ses->ses_status == SES_GOOD) {
		spin_unlock(&ses->chan_lock);
		spin_unlock(&ses->ses_lock);

		/* this means that we only need to tree connect */
		if (tcon->need_reconnect)
			goto skip_sess_setup;

		mutex_unlock(&ses->session_mutex);
		goto out;
	}
	spin_unlock(&ses->chan_lock);
	spin_unlock(&ses->ses_lock);

	rc = cifs_negotiate_protocol(0, ses, server);
	if (rc) {
		mutex_unlock(&ses->session_mutex);
		if (!tcon->retry)
			return -EHOSTDOWN;
		goto again;
	}
	rc = cifs_setup_session(0, ses, server, ses->local_nls);
	if ((rc == -EACCES) || (rc == -EHOSTDOWN) || (rc == -EKEYREVOKED)) {
		/*
		 * Try alternate password for next reconnect if an alternate
		 * password is available.
		 */
		if (ses->password2)
			swap(ses->password2, ses->password);
	}

	/* do we need to reconnect tcon? */
	if (rc || !tcon->need_reconnect) {
		mutex_unlock(&ses->session_mutex);
		goto out;
	}

skip_sess_setup:
	cifs_mark_open_files_invalid(tcon);
	rc = cifs_tree_connect(0, tcon);
	mutex_unlock(&ses->session_mutex);
	cifs_dbg(FYI, "reconnect tcon rc = %d\n", rc);

	if (rc) {
		pr_warn_once("reconnect tcon failed rc = %d\n", rc);
		goto out;
	}

	atomic_inc(&tconInfoReconnectCount);

	/* tell server Unix caps we support */
	if (cap_unix(ses))
		reset_cifs_unix_caps(0, tcon, NULL, NULL);

	/*
	 * Removed call to reopen open files here. It is safer (and faster) to
	 * reopen files one at a time as needed in read and write.
	 *
	 * FIXME: what about file locks? don't we need to reclaim them ASAP?
	 */

out:
	/*
	 * Check if handle based operation so we know whether we can continue
	 * or not without returning to caller to reset file handle
	 */
	switch (smb_command) {
	case SMB_COM_READ_ANDX:
	case SMB_COM_WRITE_ANDX:
	case SMB_COM_CLOSE:
	case SMB_COM_FIND_CLOSE2:
	case SMB_COM_LOCKING_ANDX:
		rc = -EAGAIN;
	}

	return rc;
}

/* Allocate and return pointer to an SMB request buffer, and set basic
   SMB information in the SMB header.  If the return code is zero, this
   function must have filled in request_buf pointer */
static int
small_smb_init(int smb_command, int wct, struct cifs_tcon *tcon,
		void **request_buf)
{
	int rc;

	rc = cifs_reconnect_tcon(tcon, smb_command);
	if (rc)
		return rc;

	*request_buf = cifs_small_buf_get();
	if (*request_buf == NULL) {
		/* BB should we add a retry in here if not a writepage? */
		return -ENOMEM;
	}

	header_assemble((struct smb_hdr *) *request_buf, smb_command,
			tcon, wct);

	if (tcon != NULL)
		cifs_stats_inc(&tcon->num_smbs_sent);

	return 0;
}

int
small_smb_init_no_tc(const int smb_command, const int wct,
		     struct cifs_ses *ses, void **request_buf)
{
	int rc;
	struct smb_hdr *buffer;

	rc = small_smb_init(smb_command, wct, NULL, request_buf);
	if (rc)
		return rc;

	buffer = (struct smb_hdr *)*request_buf;
	buffer->Mid = get_next_mid(ses->server);
	if (ses->capabilities & CAP_UNICODE)
		buffer->Flags2 |= SMBFLG2_UNICODE;
	if (ses->capabilities & CAP_STATUS32)
		buffer->Flags2 |= SMBFLG2_ERR_STATUS;

	/* uid, tid can stay at zero as set in header assemble */

	/* BB add support for turning on the signing when
	this function is used after 1st of session setup requests */

	return rc;
}

/* If the return code is zero, this function must fill in request_buf pointer */
static int
__smb_init(int smb_command, int wct, struct cifs_tcon *tcon,
			void **request_buf, void **response_buf)
{
	*request_buf = cifs_buf_get();
	if (*request_buf == NULL) {
		/* BB should we add a retry in here if not a writepage? */
		return -ENOMEM;
	}
    /* Although the original thought was we needed the response buf for  */
    /* potential retries of smb operations it turns out we can determine */
    /* from the mid flags when the request buffer can be resent without  */
    /* having to use a second distinct buffer for the response */
	if (response_buf)
		*response_buf = *request_buf;

	header_assemble((struct smb_hdr *) *request_buf, smb_command, tcon,
			wct);

	if (tcon != NULL)
		cifs_stats_inc(&tcon->num_smbs_sent);

	return 0;
}

/* If the return code is zero, this function must fill in request_buf pointer */
static int
smb_init(int smb_command, int wct, struct cifs_tcon *tcon,
	 void **request_buf, void **response_buf)
{
	int rc;

	rc = cifs_reconnect_tcon(tcon, smb_command);
	if (rc)
		return rc;

	return __smb_init(smb_command, wct, tcon, request_buf, response_buf);
}

static int
smb_init_no_reconnect(int smb_command, int wct, struct cifs_tcon *tcon,
			void **request_buf, void **response_buf)
{
	spin_lock(&tcon->ses->chan_lock);
	if (cifs_chan_needs_reconnect(tcon->ses, tcon->ses->server) ||
	    tcon->need_reconnect) {
		spin_unlock(&tcon->ses->chan_lock);
		return -EHOSTDOWN;
	}
	spin_unlock(&tcon->ses->chan_lock);

	return __smb_init(smb_command, wct, tcon, request_buf, response_buf);
}

static int validate_t2(struct smb_t2_rsp *pSMB)
{
	unsigned int total_size;

	/* check for plausible wct */
	if (pSMB->hdr.WordCount < 10)
		goto vt2_err;

	/* check for parm and data offset going beyond end of smb */
	if (get_unaligned_le16(&pSMB->t2_rsp.ParameterOffset) > 1024 ||
	    get_unaligned_le16(&pSMB->t2_rsp.DataOffset) > 1024)
		goto vt2_err;

	total_size = get_unaligned_le16(&pSMB->t2_rsp.ParameterCount);
	if (total_size >= 512)
		goto vt2_err;

	/* check that bcc is at least as big as parms + data, and that it is
	 * less than negotiated smb buffer
	 */
	total_size += get_unaligned_le16(&pSMB->t2_rsp.DataCount);
	if (total_size > get_bcc(&pSMB->hdr) ||
	    total_size >= CIFSMaxBufSize + MAX_CIFS_HDR_SIZE)
		goto vt2_err;

	return 0;
vt2_err:
	cifs_dump_mem("Invalid transact2 SMB: ", (char *)pSMB,
		sizeof(struct smb_t2_rsp) + 16);
	return -EINVAL;
}

static int
decode_ext_sec_blob(struct cifs_ses *ses, NEGOTIATE_RSP *pSMBr)
{
	int	rc = 0;
	u16	count;
	char	*guid = pSMBr->u.extended_response.GUID;
	struct TCP_Server_Info *server = ses->server;

	count = get_bcc(&pSMBr->hdr);
	if (count < SMB1_CLIENT_GUID_SIZE)
		return -EIO;

	spin_lock(&cifs_tcp_ses_lock);
	if (server->srv_count > 1) {
		spin_unlock(&cifs_tcp_ses_lock);
		if (memcmp(server->server_GUID, guid, SMB1_CLIENT_GUID_SIZE) != 0) {
			cifs_dbg(FYI, "server UID changed\n");
			memcpy(server->server_GUID, guid, SMB1_CLIENT_GUID_SIZE);
		}
	} else {
		spin_unlock(&cifs_tcp_ses_lock);
		memcpy(server->server_GUID, guid, SMB1_CLIENT_GUID_SIZE);
	}

	if (count == SMB1_CLIENT_GUID_SIZE) {
		server->sec_ntlmssp = true;
	} else {
		count -= SMB1_CLIENT_GUID_SIZE;
		rc = decode_negTokenInit(
			pSMBr->u.extended_response.SecurityBlob, count, server);
		if (rc != 1)
			return -EINVAL;
	}

	return 0;
}

static bool
should_set_ext_sec_flag(enum securityEnum sectype)
{
	switch (sectype) {
	case RawNTLMSSP:
	case Kerberos:
		return true;
	case Unspecified:
		if (global_secflags &
		    (CIFSSEC_MAY_KRB5 | CIFSSEC_MAY_NTLMSSP))
			return true;
		fallthrough;
	default:
		return false;
	}
}

int
CIFSSMBNegotiate(const unsigned int xid,
		 struct cifs_ses *ses,
		 struct TCP_Server_Info *server)
{
	NEGOTIATE_REQ *pSMB;
	NEGOTIATE_RSP *pSMBr;
	int rc = 0;
	int bytes_returned;
	int i;
	u16 count;

	if (!server) {
		WARN(1, "%s: server is NULL!\n", __func__);
		return -EIO;
	}

	rc = smb_init(SMB_COM_NEGOTIATE, 0, NULL /* no tcon yet */ ,
		      (void **) &pSMB, (void **) &pSMBr);
	if (rc)
		return rc;

	pSMB->hdr.Mid = get_next_mid(server);
	pSMB->hdr.Flags2 |= SMBFLG2_ERR_STATUS;

	if (ses->unicode != 0)
		pSMB->hdr.Flags2 |= SMBFLG2_UNICODE;

	if (should_set_ext_sec_flag(ses->sectype)) {
		cifs_dbg(FYI, "Requesting extended security\n");
		pSMB->hdr.Flags2 |= SMBFLG2_EXT_SEC;
	}

	count = 0;
	/*
	 * We know that all the name entries in the protocols array
	 * are short (< 16 bytes anyway) and are NUL terminated.
	 */
	for (i = 0; i < CIFS_NUM_PROT; i++) {
		size_t len = strlen(protocols[i].name) + 1;

		memcpy(&pSMB->DialectsArray[count], protocols[i].name, len);
		count += len;
	}
	inc_rfc1001_len(pSMB, count);
	pSMB->ByteCount = cpu_to_le16(count);

	rc = SendReceive(xid, ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc != 0)
		goto neg_err_exit;

	server->dialect = le16_to_cpu(pSMBr->DialectIndex);
	cifs_dbg(FYI, "Dialect: %d\n", server->dialect);
	/* Check wct = 1 error case */
	if ((pSMBr->hdr.WordCount <= 13) || (server->dialect == BAD_PROT)) {
		/* core returns wct = 1, but we do not ask for core - otherwise
		small wct just comes when dialect index is -1 indicating we
		could not negotiate a common dialect */
		rc = -EOPNOTSUPP;
		goto neg_err_exit;
	} else if (pSMBr->hdr.WordCount != 17) {
		/* unknown wct */
		rc = -EOPNOTSUPP;
		goto neg_err_exit;
	}
	/* else wct == 17, NTLM or better */

	server->sec_mode = pSMBr->SecurityMode;
	if ((server->sec_mode & SECMODE_USER) == 0)
		cifs_dbg(FYI, "share mode security\n");

	/* one byte, so no need to convert this or EncryptionKeyLen from
	   little endian */
	server->maxReq = min_t(unsigned int, le16_to_cpu(pSMBr->MaxMpxCount),
			       cifs_max_pending);
	set_credits(server, server->maxReq);
	/* probably no need to store and check maxvcs */
	server->maxBuf = le32_to_cpu(pSMBr->MaxBufferSize);
	/* set up max_read for readahead check */
	server->max_read = server->maxBuf;
	server->max_rw = le32_to_cpu(pSMBr->MaxRawSize);
	cifs_dbg(NOISY, "Max buf = %d\n", ses->server->maxBuf);
	server->capabilities = le32_to_cpu(pSMBr->Capabilities);
	server->session_key_id = pSMBr->SessionKey;
	server->timeAdj = (int)(__s16)le16_to_cpu(pSMBr->ServerTimeZone);
	server->timeAdj *= 60;

	if (pSMBr->EncryptionKeyLength == CIFS_CRYPTO_KEY_SIZE) {
		server->negflavor = CIFS_NEGFLAVOR_UNENCAP;
		memcpy(ses->server->cryptkey, pSMBr->u.EncryptionKey,
		       CIFS_CRYPTO_KEY_SIZE);
	} else if (pSMBr->hdr.Flags2 & SMBFLG2_EXT_SEC ||
			server->capabilities & CAP_EXTENDED_SECURITY) {
		server->negflavor = CIFS_NEGFLAVOR_EXTENDED;
		rc = decode_ext_sec_blob(ses, pSMBr);
	} else if (server->sec_mode & SECMODE_PW_ENCRYPT) {
		rc = -EIO; /* no crypt key only if plain text pwd */
	} else {
		server->negflavor = CIFS_NEGFLAVOR_UNENCAP;
		server->capabilities &= ~CAP_EXTENDED_SECURITY;
	}

	if (!rc)
		rc = cifs_enable_signing(server, ses->sign);
neg_err_exit:
	cifs_buf_release(pSMB);

	cifs_dbg(FYI, "negprot rc %d\n", rc);
	return rc;
}

int
CIFSSMBTDis(const unsigned int xid, struct cifs_tcon *tcon)
{
	struct smb_hdr *smb_buffer;
	int rc = 0;

	cifs_dbg(FYI, "In tree disconnect\n");

	/* BB: do we need to check this? These should never be NULL. */
	if ((tcon->ses == NULL) || (tcon->ses->server == NULL))
		return -EIO;

	/*
	 * No need to return error on this operation if tid invalidated and
	 * closed on server already e.g. due to tcp session crashing. Also,
	 * the tcon is no longer on the list, so no need to take lock before
	 * checking this.
	 */
	spin_lock(&tcon->ses->chan_lock);
	if ((tcon->need_reconnect) || CIFS_ALL_CHANS_NEED_RECONNECT(tcon->ses)) {
		spin_unlock(&tcon->ses->chan_lock);
		return -EIO;
	}
	spin_unlock(&tcon->ses->chan_lock);

	rc = small_smb_init(SMB_COM_TREE_DISCONNECT, 0, tcon,
			    (void **)&smb_buffer);
	if (rc)
		return rc;

	rc = SendReceiveNoRsp(xid, tcon->ses, (char *)smb_buffer, 0);
	cifs_small_buf_release(smb_buffer);
	if (rc)
		cifs_dbg(FYI, "Tree disconnect failed %d\n", rc);

	/* No need to return error on this operation if tid invalidated and
	   closed on server already e.g. due to tcp session crashing */
	if (rc == -EAGAIN)
		rc = 0;

	return rc;
}

/*
 * This is a no-op for now. We're not really interested in the reply, but
 * rather in the fact that the server sent one and that server->lstrp
 * gets updated.
 *
 * FIXME: maybe we should consider checking that the reply matches request?
 */
static void
cifs_echo_callback(struct mid_q_entry *mid)
{
	struct TCP_Server_Info *server = mid->callback_data;
	struct cifs_credits credits = { .value = 1, .instance = 0 };

	release_mid(mid);
	add_credits(server, &credits, CIFS_ECHO_OP);
}

int
CIFSSMBEcho(struct TCP_Server_Info *server)
{
	ECHO_REQ *smb;
	int rc = 0;
	struct kvec iov[2];
	struct smb_rqst rqst = { .rq_iov = iov,
				 .rq_nvec = 2 };

	cifs_dbg(FYI, "In echo request\n");

	rc = small_smb_init(SMB_COM_ECHO, 0, NULL, (void **)&smb);
	if (rc)
		return rc;

	if (server->capabilities & CAP_UNICODE)
		smb->hdr.Flags2 |= SMBFLG2_UNICODE;

	/* set up echo request */
	smb->hdr.Tid = 0xffff;
	smb->hdr.WordCount = 1;
	put_unaligned_le16(1, &smb->EchoCount);
	put_bcc(1, &smb->hdr);
	smb->Data[0] = 'a';
	inc_rfc1001_len(smb, 3);

	iov[0].iov_len = 4;
	iov[0].iov_base = smb;
	iov[1].iov_len = get_rfc1002_length(smb);
	iov[1].iov_base = (char *)smb + 4;

	rc = cifs_call_async(server, &rqst, NULL, cifs_echo_callback, NULL,
			     server, CIFS_NON_BLOCKING | CIFS_ECHO_OP, NULL);
	if (rc)
		cifs_dbg(FYI, "Echo request failed: %d\n", rc);

	cifs_small_buf_release(smb);

	return rc;
}

int
CIFSSMBLogoff(const unsigned int xid, struct cifs_ses *ses)
{
	LOGOFF_ANDX_REQ *pSMB;
	int rc = 0;

	cifs_dbg(FYI, "In SMBLogoff for session disconnect\n");

	/*
	 * BB: do we need to check validity of ses and server? They should
	 * always be valid since we have an active reference. If not, that
	 * should probably be a BUG()
	 */
	if (!ses || !ses->server)
		return -EIO;

	mutex_lock(&ses->session_mutex);
	spin_lock(&ses->chan_lock);
	if (CIFS_ALL_CHANS_NEED_RECONNECT(ses)) {
		spin_unlock(&ses->chan_lock);
		goto session_already_dead; /* no need to send SMBlogoff if uid
					      already closed due to reconnect */
	}
	spin_unlock(&ses->chan_lock);

	rc = small_smb_init(SMB_COM_LOGOFF_ANDX, 2, NULL, (void **)&pSMB);
	if (rc) {
		mutex_unlock(&ses->session_mutex);
		return rc;
	}

	pSMB->hdr.Mid = get_next_mid(ses->server);

	if (ses->server->sign)
		pSMB->hdr.Flags2 |= SMBFLG2_SECURITY_SIGNATURE;

	pSMB->hdr.Uid = ses->Suid;

	pSMB->AndXCommand = 0xFF;
	rc = SendReceiveNoRsp(xid, ses, (char *) pSMB, 0);
	cifs_small_buf_release(pSMB);
session_already_dead:
	mutex_unlock(&ses->session_mutex);

	/* if session dead then we do not need to do ulogoff,
		since server closed smb session, no sense reporting
		error */
	if (rc == -EAGAIN)
		rc = 0;
	return rc;
}

int
CIFSPOSIXDelFile(const unsigned int xid, struct cifs_tcon *tcon,
		 const char *fileName, __u16 type,
		 const struct nls_table *nls_codepage, int remap)
{
	TRANSACTION2_SPI_REQ *pSMB = NULL;
	TRANSACTION2_SPI_RSP *pSMBr = NULL;
	struct unlink_psx_rq *pRqD;
	int name_len;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, param_offset, offset, byte_count;

	cifs_dbg(FYI, "In POSIX delete\n");
PsxDelete:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, fileName,
				       PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, fileName);
	}

	params = 6 + name_len;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	pSMB->MaxDataCount = 0; /* BB double check this with jra */
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;

	/* Setup pointer to Request Data (inode type).
	 * Note that SMB offsets are from the beginning of SMB which is 4 bytes
	 * in, after RFC1001 field
	 */
	pRqD = (struct unlink_psx_rq *)((char *)(pSMB) + offset + 4);
	pRqD->type = cpu_to_le16(type);
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	byte_count = 3 /* pad */  + params + sizeof(struct unlink_psx_rq);

	pSMB->DataCount = cpu_to_le16(sizeof(struct unlink_psx_rq));
	pSMB->TotalDataCount = cpu_to_le16(sizeof(struct unlink_psx_rq));
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_POSIX_UNLINK);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc)
		cifs_dbg(FYI, "Posix delete returned %d\n", rc);
	cifs_buf_release(pSMB);

	cifs_stats_inc(&tcon->stats.cifs_stats.num_deletes);

	if (rc == -EAGAIN)
		goto PsxDelete;

	return rc;
}

int
CIFSSMBDelFile(const unsigned int xid, struct cifs_tcon *tcon, const char *name,
	       struct cifs_sb_info *cifs_sb, struct dentry *dentry)
{
	DELETE_FILE_REQ *pSMB = NULL;
	DELETE_FILE_RSP *pSMBr = NULL;
	int rc = 0;
	int bytes_returned;
	int name_len;
	int remap = cifs_remap(cifs_sb);

DelFileRetry:
	rc = smb_init(SMB_COM_DELETE, 1, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len = cifsConvertToUTF16((__le16 *) pSMB->fileName, name,
					      PATH_MAX, cifs_sb->local_nls,
					      remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->fileName, name);
	}
	pSMB->SearchAttributes =
	    cpu_to_le16(ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM);
	pSMB->BufferFormat = 0x04;
	inc_rfc1001_len(pSMB, name_len + 1);
	pSMB->ByteCount = cpu_to_le16(name_len + 1);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_deletes);
	if (rc)
		cifs_dbg(FYI, "Error in RMFile = %d\n", rc);

	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto DelFileRetry;

	return rc;
}

int
CIFSSMBRmDir(const unsigned int xid, struct cifs_tcon *tcon, const char *name,
	     struct cifs_sb_info *cifs_sb)
{
	DELETE_DIRECTORY_REQ *pSMB = NULL;
	DELETE_DIRECTORY_RSP *pSMBr = NULL;
	int rc = 0;
	int bytes_returned;
	int name_len;
	int remap = cifs_remap(cifs_sb);

	cifs_dbg(FYI, "In CIFSSMBRmDir\n");
RmDirRetry:
	rc = smb_init(SMB_COM_DELETE_DIRECTORY, 0, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len = cifsConvertToUTF16((__le16 *) pSMB->DirName, name,
					      PATH_MAX, cifs_sb->local_nls,
					      remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->DirName, name);
	}

	pSMB->BufferFormat = 0x04;
	inc_rfc1001_len(pSMB, name_len + 1);
	pSMB->ByteCount = cpu_to_le16(name_len + 1);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_rmdirs);
	if (rc)
		cifs_dbg(FYI, "Error in RMDir = %d\n", rc);

	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto RmDirRetry;
	return rc;
}

int
CIFSSMBMkDir(const unsigned int xid, struct inode *inode, umode_t mode,
	     struct cifs_tcon *tcon, const char *name,
	     struct cifs_sb_info *cifs_sb)
{
	int rc = 0;
	CREATE_DIRECTORY_REQ *pSMB = NULL;
	CREATE_DIRECTORY_RSP *pSMBr = NULL;
	int bytes_returned;
	int name_len;
	int remap = cifs_remap(cifs_sb);

	cifs_dbg(FYI, "In CIFSSMBMkDir\n");
MkDirRetry:
	rc = smb_init(SMB_COM_CREATE_DIRECTORY, 0, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len = cifsConvertToUTF16((__le16 *) pSMB->DirName, name,
					      PATH_MAX, cifs_sb->local_nls,
					      remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->DirName, name);
	}

	pSMB->BufferFormat = 0x04;
	inc_rfc1001_len(pSMB, name_len + 1);
	pSMB->ByteCount = cpu_to_le16(name_len + 1);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_mkdirs);
	if (rc)
		cifs_dbg(FYI, "Error in Mkdir = %d\n", rc);

	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto MkDirRetry;
	return rc;
}

int
CIFSPOSIXCreate(const unsigned int xid, struct cifs_tcon *tcon,
		__u32 posix_flags, __u64 mode, __u16 *netfid,
		FILE_UNIX_BASIC_INFO *pRetData, __u32 *pOplock,
		const char *name, const struct nls_table *nls_codepage,
		int remap)
{
	TRANSACTION2_SPI_REQ *pSMB = NULL;
	TRANSACTION2_SPI_RSP *pSMBr = NULL;
	int name_len;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, param_offset, offset, byte_count, count;
	OPEN_PSX_REQ *pdata;
	OPEN_PSX_RSP *psx_rsp;

	cifs_dbg(FYI, "In POSIX Create\n");
PsxCreat:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, name,
				       PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, name);
	}

	params = 6 + name_len;
	count = sizeof(OPEN_PSX_REQ);
	pSMB->MaxParameterCount = cpu_to_le16(2);
	pSMB->MaxDataCount = cpu_to_le16(1000);	/* large enough */
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;
	/* SMB offsets are from the beginning of SMB which is 4 bytes in, after RFC1001 field */
	pdata = (OPEN_PSX_REQ *)((char *)(pSMB) + offset + 4);
	pdata->Level = cpu_to_le16(SMB_QUERY_FILE_UNIX_BASIC);
	pdata->Permissions = cpu_to_le64(mode);
	pdata->PosixOpenFlags = cpu_to_le32(posix_flags);
	pdata->OpenFlags =  cpu_to_le32(*pOplock);
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	byte_count = 3 /* pad */  + params + count;

	pSMB->DataCount = cpu_to_le16(count);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_POSIX_OPEN);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Posix create returned %d\n", rc);
		goto psx_create_err;
	}

	cifs_dbg(FYI, "copying inode info\n");
	rc = validate_t2((struct smb_t2_rsp *)pSMBr);

	if (rc || get_bcc(&pSMBr->hdr) < sizeof(OPEN_PSX_RSP)) {
		rc = -EIO;	/* bad smb */
		goto psx_create_err;
	}

	/* copy return information to pRetData */
	psx_rsp = (OPEN_PSX_RSP *)((char *) &pSMBr->hdr.Protocol
			+ le16_to_cpu(pSMBr->t2.DataOffset));

	*pOplock = le16_to_cpu(psx_rsp->OplockFlags);
	if (netfid)
		*netfid = psx_rsp->Fid;   /* cifs fid stays in le */
	/* Let caller know file was created so we can set the mode. */
	/* Do we care about the CreateAction in any other cases? */
	if (cpu_to_le32(FILE_CREATE) == psx_rsp->CreateAction)
		*pOplock |= CIFS_CREATE_ACTION;
	/* check to make sure response data is there */
	if (psx_rsp->ReturnedLevel != cpu_to_le16(SMB_QUERY_FILE_UNIX_BASIC)) {
		pRetData->Type = cpu_to_le32(-1); /* unknown */
		cifs_dbg(NOISY, "unknown type\n");
	} else {
		if (get_bcc(&pSMBr->hdr) < sizeof(OPEN_PSX_RSP)
					+ sizeof(FILE_UNIX_BASIC_INFO)) {
			cifs_dbg(VFS, "Open response data too small\n");
			pRetData->Type = cpu_to_le32(-1);
			goto psx_create_err;
		}
		memcpy((char *) pRetData,
			(char *)psx_rsp + sizeof(OPEN_PSX_RSP),
			sizeof(FILE_UNIX_BASIC_INFO));
	}

psx_create_err:
	cifs_buf_release(pSMB);

	if (posix_flags & SMB_O_DIRECTORY)
		cifs_stats_inc(&tcon->stats.cifs_stats.num_posixmkdirs);
	else
		cifs_stats_inc(&tcon->stats.cifs_stats.num_posixopens);

	if (rc == -EAGAIN)
		goto PsxCreat;

	return rc;
}

static __u16 convert_disposition(int disposition)
{
	__u16 ofun = 0;

	switch (disposition) {
		case FILE_SUPERSEDE:
			ofun = SMBOPEN_OCREATE | SMBOPEN_OTRUNC;
			break;
		case FILE_OPEN:
			ofun = SMBOPEN_OAPPEND;
			break;
		case FILE_CREATE:
			ofun = SMBOPEN_OCREATE;
			break;
		case FILE_OPEN_IF:
			ofun = SMBOPEN_OCREATE | SMBOPEN_OAPPEND;
			break;
		case FILE_OVERWRITE:
			ofun = SMBOPEN_OTRUNC;
			break;
		case FILE_OVERWRITE_IF:
			ofun = SMBOPEN_OCREATE | SMBOPEN_OTRUNC;
			break;
		default:
			cifs_dbg(FYI, "unknown disposition %d\n", disposition);
			ofun =  SMBOPEN_OAPPEND; /* regular open */
	}
	return ofun;
}

static int
access_flags_to_smbopen_mode(const int access_flags)
{
	/*
	 * SYSTEM_SECURITY grants both read and write access to SACL, treat is as read/write.
	 * MAXIMUM_ALLOWED grants as many access as possible, so treat it as read/write too.
	 * SYNCHRONIZE as is does not grant any specific access, so do not check its mask.
	 * If only SYNCHRONIZE bit is specified then fallback to read access.
	 */
	bool with_write_flags = access_flags & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
						FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | DELETE |
						WRITE_DAC | WRITE_OWNER | SYSTEM_SECURITY |
						MAXIMUM_ALLOWED | GENERIC_WRITE | GENERIC_ALL);
	bool with_read_flags = access_flags & (FILE_READ_DATA | FILE_READ_EA | FILE_EXECUTE |
						FILE_READ_ATTRIBUTES | READ_CONTROL |
						SYSTEM_SECURITY | MAXIMUM_ALLOWED | GENERIC_ALL |
						GENERIC_EXECUTE | GENERIC_READ);
	bool with_execute_flags = access_flags & (FILE_EXECUTE | MAXIMUM_ALLOWED | GENERIC_ALL |
						GENERIC_EXECUTE);

	if (with_write_flags && with_read_flags)
		return SMBOPEN_READWRITE;
	else if (with_write_flags)
		return SMBOPEN_WRITE;
	else if (with_execute_flags)
		return SMBOPEN_EXECUTE;
	else
		return SMBOPEN_READ;
}

int
SMBLegacyOpen(const unsigned int xid, struct cifs_tcon *tcon,
	    const char *fileName, const int openDisposition,
	    const int access_flags, const int create_options, __u16 *netfid,
	    int *pOplock, FILE_ALL_INFO *pfile_info,
	    const struct nls_table *nls_codepage, int remap)
{
	int rc;
	OPENX_REQ *pSMB = NULL;
	OPENX_RSP *pSMBr = NULL;
	int bytes_returned;
	int name_len;
	__u16 count;

OldOpenRetry:
	rc = smb_init(SMB_COM_OPEN_ANDX, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	pSMB->AndXCommand = 0xFF;       /* none */

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		count = 1;      /* account for one byte pad to word boundary */
		name_len =
		   cifsConvertToUTF16((__le16 *) (pSMB->fileName + 1),
				      fileName, PATH_MAX, nls_codepage, remap);
		name_len++;     /* trailing null */
		name_len *= 2;
	} else {
		count = 0;      /* no pad */
		name_len = copy_path_name(pSMB->fileName, fileName);
	}
	if (*pOplock & REQ_OPLOCK)
		pSMB->OpenFlags = cpu_to_le16(REQ_OPLOCK);
	else if (*pOplock & REQ_BATCHOPLOCK)
		pSMB->OpenFlags = cpu_to_le16(REQ_BATCHOPLOCK);

	pSMB->OpenFlags |= cpu_to_le16(REQ_MORE_INFO);
	pSMB->Mode = cpu_to_le16(access_flags_to_smbopen_mode(access_flags));
	pSMB->Mode |= cpu_to_le16(0x40); /* deny none */
	/* set file as system file if special file such as fifo,
	 * socket, char or block and server expecting SFU style and
	   no Unix extensions */

	if (create_options & CREATE_OPTION_SPECIAL)
		pSMB->FileAttributes = cpu_to_le16(ATTR_SYSTEM);
	else /* BB FIXME BB */
		pSMB->FileAttributes = cpu_to_le16(0/*ATTR_NORMAL*/);

	if (create_options & CREATE_OPTION_READONLY)
		pSMB->FileAttributes |= cpu_to_le16(ATTR_READONLY);

	/* BB FIXME BB */
/*	pSMB->CreateOptions = cpu_to_le32(create_options &
						 CREATE_OPTIONS_MASK); */
	/* BB FIXME END BB */

	pSMB->Sattr = cpu_to_le16(ATTR_HIDDEN | ATTR_SYSTEM | ATTR_DIRECTORY);
	pSMB->OpenFunction = cpu_to_le16(convert_disposition(openDisposition));
	count += name_len;
	inc_rfc1001_len(pSMB, count);

	pSMB->ByteCount = cpu_to_le16(count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			(struct smb_hdr *)pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_opens);
	if (rc) {
		cifs_dbg(FYI, "Error in Open = %d\n", rc);
	} else {
	/* BB verify if wct == 15 */

/*		*pOplock = pSMBr->OplockLevel; */ /* BB take from action field*/

		*netfid = pSMBr->Fid;   /* cifs fid stays in le */
		/* Let caller know file was created so we can set the mode. */
		/* Do we care about the CreateAction in any other cases? */
	/* BB FIXME BB */
/*		if (cpu_to_le32(FILE_CREATE) == pSMBr->CreateAction)
			*pOplock |= CIFS_CREATE_ACTION; */
	/* BB FIXME END */

		if (pfile_info) {
			pfile_info->CreationTime = 0; /* BB convert CreateTime*/
			pfile_info->LastAccessTime = 0; /* BB fixme */
			pfile_info->LastWriteTime = 0; /* BB fixme */
			pfile_info->ChangeTime = 0;  /* BB fixme */
			pfile_info->Attributes =
				cpu_to_le32(le16_to_cpu(pSMBr->FileAttributes));
			/* the file_info buf is endian converted by caller */
			pfile_info->AllocationSize =
				cpu_to_le64(le32_to_cpu(pSMBr->EndOfFile));
			pfile_info->EndOfFile = pfile_info->AllocationSize;
			pfile_info->NumberOfLinks = cpu_to_le32(1);
			pfile_info->DeletePending = 0;
		}
	}

	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto OldOpenRetry;
	return rc;
}

int
CIFS_open(const unsigned int xid, struct cifs_open_parms *oparms, int *oplock,
	  FILE_ALL_INFO *buf)
{
	int rc;
	OPEN_REQ *req = NULL;
	OPEN_RSP *rsp = NULL;
	int bytes_returned;
	int name_len;
	__u16 count;
	struct cifs_sb_info *cifs_sb = oparms->cifs_sb;
	struct cifs_tcon *tcon = oparms->tcon;
	int remap = cifs_remap(cifs_sb);
	const struct nls_table *nls = cifs_sb->local_nls;
	int create_options = oparms->create_options;
	int desired_access = oparms->desired_access;
	int disposition = oparms->disposition;
	const char *path = oparms->path;

openRetry:
	rc = smb_init(SMB_COM_NT_CREATE_ANDX, 24, tcon, (void **)&req,
		      (void **)&rsp);
	if (rc)
		return rc;

	/* no commands go after this */
	req->AndXCommand = 0xFF;

	if (req->hdr.Flags2 & SMBFLG2_UNICODE) {
		/* account for one byte pad to word boundary */
		count = 1;
		name_len = cifsConvertToUTF16((__le16 *)(req->fileName + 1),
					      path, PATH_MAX, nls, remap);
		/* trailing null */
		name_len++;
		name_len *= 2;
		req->NameLength = cpu_to_le16(name_len);
	} else {
		/* BB improve check for buffer overruns BB */
		/* no pad */
		count = 0;
		name_len = copy_path_name(req->fileName, path);
		req->NameLength = cpu_to_le16(name_len);
	}

	if (*oplock & REQ_OPLOCK)
		req->OpenFlags = cpu_to_le32(REQ_OPLOCK);
	else if (*oplock & REQ_BATCHOPLOCK)
		req->OpenFlags = cpu_to_le32(REQ_BATCHOPLOCK);

	req->DesiredAccess = cpu_to_le32(desired_access);
	req->AllocationSize = 0;

	/*
	 * Set file as system file if special file such as fifo, socket, char
	 * or block and server expecting SFU style and no Unix extensions.
	 */
	if (create_options & CREATE_OPTION_SPECIAL)
		req->FileAttributes = cpu_to_le32(ATTR_SYSTEM);
	else
		req->FileAttributes = cpu_to_le32(ATTR_NORMAL);

	/*
	 * XP does not handle ATTR_POSIX_SEMANTICS but it helps speed up case
	 * sensitive checks for other servers such as Samba.
	 */
	if (tcon->ses->capabilities & CAP_UNIX)
		req->FileAttributes |= cpu_to_le32(ATTR_POSIX_SEMANTICS);

	if (create_options & CREATE_OPTION_READONLY)
		req->FileAttributes |= cpu_to_le32(ATTR_READONLY);

	req->ShareAccess = cpu_to_le32(FILE_SHARE_ALL);
	req->CreateDisposition = cpu_to_le32(disposition);
	req->CreateOptions = cpu_to_le32(create_options & CREATE_OPTIONS_MASK);

	/* BB Experiment with various impersonation levels and verify */
	req->ImpersonationLevel = cpu_to_le32(SECURITY_IMPERSONATION);
	req->SecurityFlags = SECURITY_CONTEXT_TRACKING|SECURITY_EFFECTIVE_ONLY;

	count += name_len;
	inc_rfc1001_len(req, count);

	req->ByteCount = cpu_to_le16(count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *)req,
			 (struct smb_hdr *)rsp, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_opens);
	if (rc) {
		cifs_dbg(FYI, "Error in Open = %d\n", rc);
		cifs_buf_release(req);
		if (rc == -EAGAIN)
			goto openRetry;
		return rc;
	}

	/* 1 byte no need to le_to_cpu */
	*oplock = rsp->OplockLevel;
	/* cifs fid stays in le */
	oparms->fid->netfid = rsp->Fid;
	oparms->fid->access = desired_access;

	/* Let caller know file was created so we can set the mode. */
	/* Do we care about the CreateAction in any other cases? */
	if (cpu_to_le32(FILE_CREATE) == rsp->CreateAction)
		*oplock |= CIFS_CREATE_ACTION;

	if (buf) {
		/* copy commonly used attributes */
		memcpy(&buf->common_attributes,
		       &rsp->common_attributes,
		       sizeof(buf->common_attributes));
		/* the file_info buf is endian converted by caller */
		buf->AllocationSize = rsp->AllocationSize;
		buf->EndOfFile = rsp->EndOfFile;
		buf->NumberOfLinks = cpu_to_le32(1);
		buf->DeletePending = 0;
	}

	cifs_buf_release(req);
	return rc;
}

static void
cifs_readv_callback(struct mid_q_entry *mid)
{
	struct cifs_io_subrequest *rdata = mid->callback_data;
	struct netfs_inode *ictx = netfs_inode(rdata->rreq->inode);
	struct cifs_tcon *tcon = tlink_tcon(rdata->req->cfile->tlink);
	struct TCP_Server_Info *server = tcon->ses->server;
	struct smb_rqst rqst = { .rq_iov = rdata->iov,
				 .rq_nvec = 2,
				 .rq_iter = rdata->subreq.io_iter };
	struct cifs_credits credits = {
		.value = 1,
		.instance = 0,
		.rreq_debug_id = rdata->rreq->debug_id,
		.rreq_debug_index = rdata->subreq.debug_index,
	};

	cifs_dbg(FYI, "%s: mid=%llu state=%d result=%d bytes=%zu\n",
		 __func__, mid->mid, mid->mid_state, rdata->result,
		 rdata->subreq.len);

	switch (mid->mid_state) {
	case MID_RESPONSE_RECEIVED:
		/* result already set, check signature */
		if (server->sign) {
			int rc = 0;

			iov_iter_truncate(&rqst.rq_iter, rdata->got_bytes);
			rc = cifs_verify_signature(&rqst, server,
						  mid->sequence_number);
			if (rc)
				cifs_dbg(VFS, "SMB signature verification returned error = %d\n",
					 rc);
		}
		/* FIXME: should this be counted toward the initiating task? */
		task_io_account_read(rdata->got_bytes);
		cifs_stats_bytes_read(tcon, rdata->got_bytes);
		break;
	case MID_REQUEST_SUBMITTED:
		trace_netfs_sreq(&rdata->subreq, netfs_sreq_trace_io_req_submitted);
		goto do_retry;
	case MID_RETRY_NEEDED:
		trace_netfs_sreq(&rdata->subreq, netfs_sreq_trace_io_retry_needed);
do_retry:
		__set_bit(NETFS_SREQ_NEED_RETRY, &rdata->subreq.flags);
		rdata->result = -EAGAIN;
		if (server->sign && rdata->got_bytes)
			/* reset bytes number since we can not check a sign */
			rdata->got_bytes = 0;
		/* FIXME: should this be counted toward the initiating task? */
		task_io_account_read(rdata->got_bytes);
		cifs_stats_bytes_read(tcon, rdata->got_bytes);
		break;
	case MID_RESPONSE_MALFORMED:
		trace_netfs_sreq(&rdata->subreq, netfs_sreq_trace_io_malformed);
		rdata->result = -EIO;
		break;
	default:
		trace_netfs_sreq(&rdata->subreq, netfs_sreq_trace_io_unknown);
		rdata->result = -EIO;
		break;
	}

	if (rdata->result == -ENODATA) {
		rdata->result = 0;
		__set_bit(NETFS_SREQ_HIT_EOF, &rdata->subreq.flags);
	} else {
		size_t trans = rdata->subreq.transferred + rdata->got_bytes;
		if (trans < rdata->subreq.len &&
		    rdata->subreq.start + trans == ictx->remote_i_size) {
			rdata->result = 0;
			__set_bit(NETFS_SREQ_HIT_EOF, &rdata->subreq.flags);
		} else if (rdata->got_bytes > 0) {
			__set_bit(NETFS_SREQ_MADE_PROGRESS, &rdata->subreq.flags);
		}
		if (rdata->got_bytes)
			__set_bit(NETFS_SREQ_MADE_PROGRESS, &rdata->subreq.flags);
	}

	rdata->credits.value = 0;
	rdata->subreq.error = rdata->result;
	rdata->subreq.transferred += rdata->got_bytes;
	trace_netfs_sreq(&rdata->subreq, netfs_sreq_trace_io_progress);
	netfs_read_subreq_terminated(&rdata->subreq);
	release_mid(mid);
	add_credits(server, &credits, 0);
}

/* cifs_async_readv - send an async write, and set up mid to handle result */
int
cifs_async_readv(struct cifs_io_subrequest *rdata)
{
	int rc;
	READ_REQ *smb = NULL;
	int wct;
	struct cifs_tcon *tcon = tlink_tcon(rdata->req->cfile->tlink);
	struct smb_rqst rqst = { .rq_iov = rdata->iov,
				 .rq_nvec = 2 };

	cifs_dbg(FYI, "%s: offset=%llu bytes=%zu\n",
		 __func__, rdata->subreq.start, rdata->subreq.len);

	if (tcon->ses->capabilities & CAP_LARGE_FILES)
		wct = 12;
	else {
		wct = 10; /* old style read */
		if ((rdata->subreq.start >> 32) > 0)  {
			/* can not handle this big offset for old */
			return -EIO;
		}
	}

	rc = small_smb_init(SMB_COM_READ_ANDX, wct, tcon, (void **)&smb);
	if (rc)
		return rc;

	smb->hdr.Pid = cpu_to_le16((__u16)rdata->req->pid);
	smb->hdr.PidHigh = cpu_to_le16((__u16)(rdata->req->pid >> 16));

	smb->AndXCommand = 0xFF;	/* none */
	smb->Fid = rdata->req->cfile->fid.netfid;
	smb->OffsetLow = cpu_to_le32(rdata->subreq.start & 0xFFFFFFFF);
	if (wct == 12)
		smb->OffsetHigh = cpu_to_le32(rdata->subreq.start >> 32);
	smb->Remaining = 0;
	smb->MaxCount = cpu_to_le16(rdata->subreq.len & 0xFFFF);
	smb->MaxCountHigh = cpu_to_le32(rdata->subreq.len >> 16);
	if (wct == 12)
		smb->ByteCount = 0;
	else {
		/* old style read */
		struct smb_com_readx_req *smbr =
			(struct smb_com_readx_req *)smb;
		smbr->ByteCount = 0;
	}

	/* 4 for RFC1001 length + 1 for BCC */
	rdata->iov[0].iov_base = smb;
	rdata->iov[0].iov_len = 4;
	rdata->iov[1].iov_base = (char *)smb + 4;
	rdata->iov[1].iov_len = get_rfc1002_length(smb);

	rc = cifs_call_async(tcon->ses->server, &rqst, cifs_readv_receive,
			     cifs_readv_callback, NULL, rdata, 0, NULL);

	if (rc == 0)
		cifs_stats_inc(&tcon->stats.cifs_stats.num_reads);
	cifs_small_buf_release(smb);
	return rc;
}

int
CIFSSMBRead(const unsigned int xid, struct cifs_io_parms *io_parms,
	    unsigned int *nbytes, char **buf, int *pbuf_type)
{
	int rc = -EACCES;
	READ_REQ *pSMB = NULL;
	READ_RSP *pSMBr = NULL;
	char *pReadData = NULL;
	int wct;
	int resp_buf_type = 0;
	struct kvec iov[1];
	struct kvec rsp_iov;
	__u32 pid = io_parms->pid;
	__u16 netfid = io_parms->netfid;
	__u64 offset = io_parms->offset;
	struct cifs_tcon *tcon = io_parms->tcon;
	unsigned int count = io_parms->length;

	cifs_dbg(FYI, "Reading %d bytes on fid %d\n", count, netfid);
	if (tcon->ses->capabilities & CAP_LARGE_FILES)
		wct = 12;
	else {
		wct = 10; /* old style read */
		if ((offset >> 32) > 0)  {
			/* can not handle this big offset for old */
			return -EIO;
		}
	}

	*nbytes = 0;
	rc = small_smb_init(SMB_COM_READ_ANDX, wct, tcon, (void **) &pSMB);
	if (rc)
		return rc;

	pSMB->hdr.Pid = cpu_to_le16((__u16)pid);
	pSMB->hdr.PidHigh = cpu_to_le16((__u16)(pid >> 16));

	/* tcon and ses pointer are checked in smb_init */
	if (tcon->ses->server == NULL)
		return -ECONNABORTED;

	pSMB->AndXCommand = 0xFF;       /* none */
	pSMB->Fid = netfid;
	pSMB->OffsetLow = cpu_to_le32(offset & 0xFFFFFFFF);
	if (wct == 12)
		pSMB->OffsetHigh = cpu_to_le32(offset >> 32);

	pSMB->Remaining = 0;
	pSMB->MaxCount = cpu_to_le16(count & 0xFFFF);
	pSMB->MaxCountHigh = cpu_to_le32(count >> 16);
	if (wct == 12)
		pSMB->ByteCount = 0;  /* no need to do le conversion since 0 */
	else {
		/* old style read */
		struct smb_com_readx_req *pSMBW =
			(struct smb_com_readx_req *)pSMB;
		pSMBW->ByteCount = 0;
	}

	iov[0].iov_base = (char *)pSMB;
	iov[0].iov_len = be32_to_cpu(pSMB->hdr.smb_buf_length) + 4;
	rc = SendReceive2(xid, tcon->ses, iov, 1, &resp_buf_type,
			  CIFS_LOG_ERROR, &rsp_iov);
	cifs_small_buf_release(pSMB);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_reads);
	pSMBr = (READ_RSP *)rsp_iov.iov_base;
	if (rc) {
		cifs_dbg(VFS, "Send error in read = %d\n", rc);
	} else {
		int data_length = le16_to_cpu(pSMBr->DataLengthHigh);
		data_length = data_length << 16;
		data_length += le16_to_cpu(pSMBr->DataLength);
		*nbytes = data_length;

		/*check that DataLength would not go beyond end of SMB */
		if ((data_length > CIFSMaxBufSize)
				|| (data_length > count)) {
			cifs_dbg(FYI, "bad length %d for count %d\n",
				 data_length, count);
			rc = -EIO;
			*nbytes = 0;
		} else {
			pReadData = (char *) (&pSMBr->hdr.Protocol) +
					le16_to_cpu(pSMBr->DataOffset);
/*			if (rc = copy_to_user(buf, pReadData, data_length)) {
				cifs_dbg(VFS, "Faulting on read rc = %d\n",rc);
				rc = -EFAULT;
			}*/ /* can not use copy_to_user when using page cache*/
			if (*buf)
				memcpy(*buf, pReadData, data_length);
		}
	}

	if (*buf) {
		free_rsp_buf(resp_buf_type, rsp_iov.iov_base);
	} else if (resp_buf_type != CIFS_NO_BUFFER) {
		/* return buffer to caller to free */
		*buf = rsp_iov.iov_base;
		if (resp_buf_type == CIFS_SMALL_BUFFER)
			*pbuf_type = CIFS_SMALL_BUFFER;
		else if (resp_buf_type == CIFS_LARGE_BUFFER)
			*pbuf_type = CIFS_LARGE_BUFFER;
	} /* else no valid buffer on return - leave as null */

	/* Note: On -EAGAIN error only caller can retry on handle based calls
		since file handle passed in no longer valid */
	return rc;
}


int
CIFSSMBWrite(const unsigned int xid, struct cifs_io_parms *io_parms,
	     unsigned int *nbytes, const char *buf)
{
	int rc = -EACCES;
	WRITE_REQ *pSMB = NULL;
	WRITE_RSP *pSMBr = NULL;
	int bytes_returned, wct;
	__u32 bytes_sent;
	__u16 byte_count;
	__u32 pid = io_parms->pid;
	__u16 netfid = io_parms->netfid;
	__u64 offset = io_parms->offset;
	struct cifs_tcon *tcon = io_parms->tcon;
	unsigned int count = io_parms->length;

	*nbytes = 0;

	/* cifs_dbg(FYI, "write at %lld %d bytes\n", offset, count);*/
	if (tcon->ses == NULL)
		return -ECONNABORTED;

	if (tcon->ses->capabilities & CAP_LARGE_FILES)
		wct = 14;
	else {
		wct = 12;
		if ((offset >> 32) > 0) {
			/* can not handle big offset for old srv */
			return -EIO;
		}
	}

	rc = smb_init(SMB_COM_WRITE_ANDX, wct, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	pSMB->hdr.Pid = cpu_to_le16((__u16)pid);
	pSMB->hdr.PidHigh = cpu_to_le16((__u16)(pid >> 16));

	/* tcon and ses pointer are checked in smb_init */
	if (tcon->ses->server == NULL)
		return -ECONNABORTED;

	pSMB->AndXCommand = 0xFF;	/* none */
	pSMB->Fid = netfid;
	pSMB->OffsetLow = cpu_to_le32(offset & 0xFFFFFFFF);
	if (wct == 14)
		pSMB->OffsetHigh = cpu_to_le32(offset >> 32);

	pSMB->Reserved = 0xFFFFFFFF;
	pSMB->WriteMode = 0;
	pSMB->Remaining = 0;

	/* Can increase buffer size if buffer is big enough in some cases ie we
	can send more if LARGE_WRITE_X capability returned by the server and if
	our buffer is big enough or if we convert to iovecs on socket writes
	and eliminate the copy to the CIFS buffer */
	if (tcon->ses->capabilities & CAP_LARGE_WRITE_X) {
		bytes_sent = min_t(const unsigned int, CIFSMaxBufSize, count);
	} else {
		bytes_sent = (tcon->ses->server->maxBuf - MAX_CIFS_HDR_SIZE)
			 & ~0xFF;
	}

	if (bytes_sent > count)
		bytes_sent = count;
	pSMB->DataOffset =
		cpu_to_le16(offsetof(struct smb_com_write_req, Data) - 4);
	if (buf)
		memcpy(pSMB->Data, buf, bytes_sent);
	else if (count != 0) {
		/* No buffer */
		cifs_buf_release(pSMB);
		return -EINVAL;
	} /* else setting file size with write of zero bytes */
	if (wct == 14)
		byte_count = bytes_sent + 1; /* pad */
	else /* wct == 12 */
		byte_count = bytes_sent + 5; /* bigger pad, smaller smb hdr */

	pSMB->DataLengthLow = cpu_to_le16(bytes_sent & 0xFFFF);
	pSMB->DataLengthHigh = cpu_to_le16(bytes_sent >> 16);
	inc_rfc1001_len(pSMB, byte_count);

	if (wct == 14)
		pSMB->ByteCount = cpu_to_le16(byte_count);
	else { /* old style write has byte count 4 bytes earlier
		  so 4 bytes pad  */
		struct smb_com_writex_req *pSMBW =
			(struct smb_com_writex_req *)pSMB;
		pSMBW->ByteCount = cpu_to_le16(byte_count);
	}

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_writes);
	if (rc) {
		cifs_dbg(FYI, "Send error in write = %d\n", rc);
	} else {
		*nbytes = le16_to_cpu(pSMBr->CountHigh);
		*nbytes = (*nbytes) << 16;
		*nbytes += le16_to_cpu(pSMBr->Count);

		/*
		 * Mask off high 16 bits when bytes written as returned by the
		 * server is greater than bytes requested by the client. Some
		 * OS/2 servers are known to set incorrect CountHigh values.
		 */
		if (*nbytes > count)
			*nbytes &= 0xFFFF;
	}

	cifs_buf_release(pSMB);

	/* Note: On -EAGAIN error only caller can retry on handle based calls
		since file handle passed in no longer valid */

	return rc;
}

/*
 * Check the mid_state and signature on received buffer (if any), and queue the
 * workqueue completion task.
 */
static void
cifs_writev_callback(struct mid_q_entry *mid)
{
	struct cifs_io_subrequest *wdata = mid->callback_data;
	struct TCP_Server_Info *server = wdata->server;
	struct cifs_tcon *tcon = tlink_tcon(wdata->req->cfile->tlink);
	WRITE_RSP *smb = (WRITE_RSP *)mid->resp_buf;
	struct cifs_credits credits = {
		.value = 1,
		.instance = 0,
		.rreq_debug_id = wdata->rreq->debug_id,
		.rreq_debug_index = wdata->subreq.debug_index,
	};
	ssize_t result;
	size_t written;

	switch (mid->mid_state) {
	case MID_RESPONSE_RECEIVED:
		result = cifs_check_receive(mid, tcon->ses->server, 0);
		if (result != 0)
			break;

		written = le16_to_cpu(smb->CountHigh);
		written <<= 16;
		written += le16_to_cpu(smb->Count);
		/*
		 * Mask off high 16 bits when bytes written as returned
		 * by the server is greater than bytes requested by the
		 * client. OS/2 servers are known to set incorrect
		 * CountHigh values.
		 */
		if (written > wdata->subreq.len)
			written &= 0xFFFF;

		if (written < wdata->subreq.len) {
			result = -ENOSPC;
		} else {
			result = written;
			if (written > 0)
				__set_bit(NETFS_SREQ_MADE_PROGRESS, &wdata->subreq.flags);
		}
		break;
	case MID_REQUEST_SUBMITTED:
		trace_netfs_sreq(&wdata->subreq, netfs_sreq_trace_io_req_submitted);
		__set_bit(NETFS_SREQ_NEED_RETRY, &wdata->subreq.flags);
		result = -EAGAIN;
		break;
	case MID_RETRY_NEEDED:
		trace_netfs_sreq(&wdata->subreq, netfs_sreq_trace_io_retry_needed);
		__set_bit(NETFS_SREQ_NEED_RETRY, &wdata->subreq.flags);
		result = -EAGAIN;
		break;
	case MID_RESPONSE_MALFORMED:
		trace_netfs_sreq(&wdata->subreq, netfs_sreq_trace_io_malformed);
		result = -EIO;
		break;
	default:
		trace_netfs_sreq(&wdata->subreq, netfs_sreq_trace_io_unknown);
		result = -EIO;
		break;
	}

	trace_smb3_rw_credits(credits.rreq_debug_id, credits.rreq_debug_index,
			      wdata->credits.value,
			      server->credits, server->in_flight,
			      0, cifs_trace_rw_credits_write_response_clear);
	wdata->credits.value = 0;
	cifs_write_subrequest_terminated(wdata, result);
	release_mid(mid);
	trace_smb3_rw_credits(credits.rreq_debug_id, credits.rreq_debug_index, 0,
			      server->credits, server->in_flight,
			      credits.value, cifs_trace_rw_credits_write_response_add);
	add_credits(tcon->ses->server, &credits, 0);
}

/* cifs_async_writev - send an async write, and set up mid to handle result */
void
cifs_async_writev(struct cifs_io_subrequest *wdata)
{
	int rc = -EACCES;
	WRITE_REQ *smb = NULL;
	int wct;
	struct cifs_tcon *tcon = tlink_tcon(wdata->req->cfile->tlink);
	struct kvec iov[2];
	struct smb_rqst rqst = { };

	if (tcon->ses->capabilities & CAP_LARGE_FILES) {
		wct = 14;
	} else {
		wct = 12;
		if (wdata->subreq.start >> 32 > 0) {
			/* can not handle big offset for old srv */
			rc = -EIO;
			goto out;
		}
	}

	rc = small_smb_init(SMB_COM_WRITE_ANDX, wct, tcon, (void **)&smb);
	if (rc)
		goto async_writev_out;

	smb->hdr.Pid = cpu_to_le16((__u16)wdata->req->pid);
	smb->hdr.PidHigh = cpu_to_le16((__u16)(wdata->req->pid >> 16));

	smb->AndXCommand = 0xFF;	/* none */
	smb->Fid = wdata->req->cfile->fid.netfid;
	smb->OffsetLow = cpu_to_le32(wdata->subreq.start & 0xFFFFFFFF);
	if (wct == 14)
		smb->OffsetHigh = cpu_to_le32(wdata->subreq.start >> 32);
	smb->Reserved = 0xFFFFFFFF;
	smb->WriteMode = 0;
	smb->Remaining = 0;

	smb->DataOffset =
	    cpu_to_le16(offsetof(struct smb_com_write_req, Data) - 4);

	/* 4 for RFC1001 length + 1 for BCC */
	iov[0].iov_len = 4;
	iov[0].iov_base = smb;
	iov[1].iov_len = get_rfc1002_length(smb) + 1;
	iov[1].iov_base = (char *)smb + 4;

	rqst.rq_iov = iov;
	rqst.rq_nvec = 2;
	rqst.rq_iter = wdata->subreq.io_iter;

	cifs_dbg(FYI, "async write at %llu %zu bytes\n",
		 wdata->subreq.start, wdata->subreq.len);

	smb->DataLengthLow = cpu_to_le16(wdata->subreq.len & 0xFFFF);
	smb->DataLengthHigh = cpu_to_le16(wdata->subreq.len >> 16);

	if (wct == 14) {
		inc_rfc1001_len(&smb->hdr, wdata->subreq.len + 1);
		put_bcc(wdata->subreq.len + 1, &smb->hdr);
	} else {
		/* wct == 12 */
		struct smb_com_writex_req *smbw =
				(struct smb_com_writex_req *)smb;
		inc_rfc1001_len(&smbw->hdr, wdata->subreq.len + 5);
		put_bcc(wdata->subreq.len + 5, &smbw->hdr);
		iov[1].iov_len += 4; /* pad bigger by four bytes */
	}

	rc = cifs_call_async(tcon->ses->server, &rqst, NULL,
			     cifs_writev_callback, NULL, wdata, 0, NULL);
	/* Can't touch wdata if rc == 0 */
	if (rc == 0)
		cifs_stats_inc(&tcon->stats.cifs_stats.num_writes);

async_writev_out:
	cifs_small_buf_release(smb);
out:
	if (rc) {
		add_credits_and_wake_if(wdata->server, &wdata->credits, 0);
		cifs_write_subrequest_terminated(wdata, rc);
	}
}

int
CIFSSMBWrite2(const unsigned int xid, struct cifs_io_parms *io_parms,
	      unsigned int *nbytes, struct kvec *iov, int n_vec)
{
	int rc;
	WRITE_REQ *pSMB = NULL;
	int wct;
	int smb_hdr_len;
	int resp_buf_type = 0;
	__u32 pid = io_parms->pid;
	__u16 netfid = io_parms->netfid;
	__u64 offset = io_parms->offset;
	struct cifs_tcon *tcon = io_parms->tcon;
	unsigned int count = io_parms->length;
	struct kvec rsp_iov;

	*nbytes = 0;

	cifs_dbg(FYI, "write2 at %lld %d bytes\n", (long long)offset, count);

	if (tcon->ses->capabilities & CAP_LARGE_FILES) {
		wct = 14;
	} else {
		wct = 12;
		if ((offset >> 32) > 0) {
			/* can not handle big offset for old srv */
			return -EIO;
		}
	}
	rc = small_smb_init(SMB_COM_WRITE_ANDX, wct, tcon, (void **) &pSMB);
	if (rc)
		return rc;

	pSMB->hdr.Pid = cpu_to_le16((__u16)pid);
	pSMB->hdr.PidHigh = cpu_to_le16((__u16)(pid >> 16));

	/* tcon and ses pointer are checked in smb_init */
	if (tcon->ses->server == NULL)
		return -ECONNABORTED;

	pSMB->AndXCommand = 0xFF;	/* none */
	pSMB->Fid = netfid;
	pSMB->OffsetLow = cpu_to_le32(offset & 0xFFFFFFFF);
	if (wct == 14)
		pSMB->OffsetHigh = cpu_to_le32(offset >> 32);
	pSMB->Reserved = 0xFFFFFFFF;
	pSMB->WriteMode = 0;
	pSMB->Remaining = 0;

	pSMB->DataOffset =
	    cpu_to_le16(offsetof(struct smb_com_write_req, Data) - 4);

	pSMB->DataLengthLow = cpu_to_le16(count & 0xFFFF);
	pSMB->DataLengthHigh = cpu_to_le16(count >> 16);
	/* header + 1 byte pad */
	smb_hdr_len = be32_to_cpu(pSMB->hdr.smb_buf_length) + 1;
	if (wct == 14)
		inc_rfc1001_len(pSMB, count + 1);
	else /* wct == 12 */
		inc_rfc1001_len(pSMB, count + 5); /* smb data starts later */
	if (wct == 14)
		pSMB->ByteCount = cpu_to_le16(count + 1);
	else /* wct == 12 */ /* bigger pad, smaller smb hdr, keep offset ok */ {
		struct smb_com_writex_req *pSMBW =
				(struct smb_com_writex_req *)pSMB;
		pSMBW->ByteCount = cpu_to_le16(count + 5);
	}
	iov[0].iov_base = pSMB;
	if (wct == 14)
		iov[0].iov_len = smb_hdr_len + 4;
	else /* wct == 12 pad bigger by four bytes */
		iov[0].iov_len = smb_hdr_len + 8;

	rc = SendReceive2(xid, tcon->ses, iov, n_vec + 1, &resp_buf_type, 0,
			  &rsp_iov);
	cifs_small_buf_release(pSMB);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_writes);
	if (rc) {
		cifs_dbg(FYI, "Send error Write2 = %d\n", rc);
	} else if (resp_buf_type == 0) {
		/* presumably this can not happen, but best to be safe */
		rc = -EIO;
	} else {
		WRITE_RSP *pSMBr = (WRITE_RSP *)rsp_iov.iov_base;
		*nbytes = le16_to_cpu(pSMBr->CountHigh);
		*nbytes = (*nbytes) << 16;
		*nbytes += le16_to_cpu(pSMBr->Count);

		/*
		 * Mask off high 16 bits when bytes written as returned by the
		 * server is greater than bytes requested by the client. OS/2
		 * servers are known to set incorrect CountHigh values.
		 */
		if (*nbytes > count)
			*nbytes &= 0xFFFF;
	}

	free_rsp_buf(resp_buf_type, rsp_iov.iov_base);

	/* Note: On -EAGAIN error only caller can retry on handle based calls
		since file handle passed in no longer valid */

	return rc;
}

int cifs_lockv(const unsigned int xid, struct cifs_tcon *tcon,
	       const __u16 netfid, const __u8 lock_type, const __u32 num_unlock,
	       const __u32 num_lock, LOCKING_ANDX_RANGE *buf)
{
	int rc = 0;
	LOCK_REQ *pSMB = NULL;
	struct kvec iov[2];
	struct kvec rsp_iov;
	int resp_buf_type;
	__u16 count;

	cifs_dbg(FYI, "cifs_lockv num lock %d num unlock %d\n",
		 num_lock, num_unlock);

	rc = small_smb_init(SMB_COM_LOCKING_ANDX, 8, tcon, (void **) &pSMB);
	if (rc)
		return rc;

	pSMB->Timeout = 0;
	pSMB->NumberOfLocks = cpu_to_le16(num_lock);
	pSMB->NumberOfUnlocks = cpu_to_le16(num_unlock);
	pSMB->LockType = lock_type;
	pSMB->AndXCommand = 0xFF; /* none */
	pSMB->Fid = netfid; /* netfid stays le */

	count = (num_unlock + num_lock) * sizeof(LOCKING_ANDX_RANGE);
	inc_rfc1001_len(pSMB, count);
	pSMB->ByteCount = cpu_to_le16(count);

	iov[0].iov_base = (char *)pSMB;
	iov[0].iov_len = be32_to_cpu(pSMB->hdr.smb_buf_length) + 4 -
			 (num_unlock + num_lock) * sizeof(LOCKING_ANDX_RANGE);
	iov[1].iov_base = (char *)buf;
	iov[1].iov_len = (num_unlock + num_lock) * sizeof(LOCKING_ANDX_RANGE);

	cifs_stats_inc(&tcon->stats.cifs_stats.num_locks);
	rc = SendReceive2(xid, tcon->ses, iov, 2, &resp_buf_type,
			  CIFS_NO_RSP_BUF, &rsp_iov);
	cifs_small_buf_release(pSMB);
	if (rc)
		cifs_dbg(FYI, "Send error in cifs_lockv = %d\n", rc);

	return rc;
}

int
CIFSSMBLock(const unsigned int xid, struct cifs_tcon *tcon,
	    const __u16 smb_file_id, const __u32 netpid, const __u64 len,
	    const __u64 offset, const __u32 numUnlock,
	    const __u32 numLock, const __u8 lockType,
	    const bool waitFlag, const __u8 oplock_level)
{
	int rc = 0;
	LOCK_REQ *pSMB = NULL;
/*	LOCK_RSP *pSMBr = NULL; */ /* No response data other than rc to parse */
	int bytes_returned;
	int flags = 0;
	__u16 count;

	cifs_dbg(FYI, "CIFSSMBLock timeout %d numLock %d\n",
		 (int)waitFlag, numLock);
	rc = small_smb_init(SMB_COM_LOCKING_ANDX, 8, tcon, (void **) &pSMB);

	if (rc)
		return rc;

	if (lockType == LOCKING_ANDX_OPLOCK_RELEASE) {
		/* no response expected */
		flags = CIFS_NO_SRV_RSP | CIFS_NON_BLOCKING | CIFS_OBREAK_OP;
		pSMB->Timeout = 0;
	} else if (waitFlag) {
		flags = CIFS_BLOCKING_OP; /* blocking operation, no timeout */
		pSMB->Timeout = cpu_to_le32(-1);/* blocking - do not time out */
	} else {
		pSMB->Timeout = 0;
	}

	pSMB->NumberOfLocks = cpu_to_le16(numLock);
	pSMB->NumberOfUnlocks = cpu_to_le16(numUnlock);
	pSMB->LockType = lockType;
	pSMB->OplockLevel = oplock_level;
	pSMB->AndXCommand = 0xFF;	/* none */
	pSMB->Fid = smb_file_id; /* netfid stays le */

	if ((numLock != 0) || (numUnlock != 0)) {
		pSMB->Locks[0].Pid = cpu_to_le16(netpid);
		/* BB where to store pid high? */
		pSMB->Locks[0].LengthLow = cpu_to_le32((u32)len);
		pSMB->Locks[0].LengthHigh = cpu_to_le32((u32)(len>>32));
		pSMB->Locks[0].OffsetLow = cpu_to_le32((u32)offset);
		pSMB->Locks[0].OffsetHigh = cpu_to_le32((u32)(offset>>32));
		count = sizeof(LOCKING_ANDX_RANGE);
	} else {
		/* oplock break */
		count = 0;
	}
	inc_rfc1001_len(pSMB, count);
	pSMB->ByteCount = cpu_to_le16(count);

	if (waitFlag)
		rc = SendReceiveBlockingLock(xid, tcon, (struct smb_hdr *) pSMB,
			(struct smb_hdr *) pSMB, &bytes_returned);
	else
		rc = SendReceiveNoRsp(xid, tcon->ses, (char *)pSMB, flags);
	cifs_small_buf_release(pSMB);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_locks);
	if (rc)
		cifs_dbg(FYI, "Send error in Lock = %d\n", rc);

	/* Note: On -EAGAIN error only caller can retry on handle based calls
	since file handle passed in no longer valid */
	return rc;
}

int
CIFSSMBPosixLock(const unsigned int xid, struct cifs_tcon *tcon,
		const __u16 smb_file_id, const __u32 netpid,
		const loff_t start_offset, const __u64 len,
		struct file_lock *pLockData, const __u16 lock_type,
		const bool waitFlag)
{
	struct smb_com_transaction2_sfi_req *pSMB  = NULL;
	struct smb_com_transaction2_sfi_rsp *pSMBr = NULL;
	struct cifs_posix_lock *parm_data;
	int rc = 0;
	int timeout = 0;
	int bytes_returned = 0;
	int resp_buf_type = 0;
	__u16 params, param_offset, offset, byte_count, count;
	struct kvec iov[1];
	struct kvec rsp_iov;

	cifs_dbg(FYI, "Posix Lock\n");

	rc = small_smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB);

	if (rc)
		return rc;

	pSMBr = (struct smb_com_transaction2_sfi_rsp *)pSMB;

	params = 6;
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_sfi_req, Fid) - 4;
	offset = param_offset + params;

	count = sizeof(struct cifs_posix_lock);
	pSMB->MaxParameterCount = cpu_to_le16(2);
	pSMB->MaxDataCount = cpu_to_le16(1000); /* BB find max SMB from sess */
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	if (pLockData)
		pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_FILE_INFORMATION);
	else
		pSMB->SubCommand = cpu_to_le16(TRANS2_SET_FILE_INFORMATION);
	byte_count = 3 /* pad */  + params + count;
	pSMB->DataCount = cpu_to_le16(count);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	/* SMB offsets are from the beginning of SMB which is 4 bytes in, after RFC1001 field */
	parm_data = (struct cifs_posix_lock *)
			(((char *)pSMB) + offset + 4);

	parm_data->lock_type = cpu_to_le16(lock_type);
	if (waitFlag) {
		timeout = CIFS_BLOCKING_OP; /* blocking operation, no timeout */
		parm_data->lock_flags = cpu_to_le16(1);
		pSMB->Timeout = cpu_to_le32(-1);
	} else
		pSMB->Timeout = 0;

	parm_data->pid = cpu_to_le32(netpid);
	parm_data->start = cpu_to_le64(start_offset);
	parm_data->length = cpu_to_le64(len);  /* normalize negative numbers */

	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->Fid = smb_file_id;
	pSMB->InformationLevel = cpu_to_le16(SMB_SET_POSIX_LOCK);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	if (waitFlag) {
		rc = SendReceiveBlockingLock(xid, tcon, (struct smb_hdr *) pSMB,
			(struct smb_hdr *) pSMBr, &bytes_returned);
	} else {
		iov[0].iov_base = (char *)pSMB;
		iov[0].iov_len = be32_to_cpu(pSMB->hdr.smb_buf_length) + 4;
		rc = SendReceive2(xid, tcon->ses, iov, 1 /* num iovecs */,
				&resp_buf_type, timeout, &rsp_iov);
		pSMBr = (struct smb_com_transaction2_sfi_rsp *)rsp_iov.iov_base;
	}
	cifs_small_buf_release(pSMB);

	if (rc) {
		cifs_dbg(FYI, "Send error in Posix Lock = %d\n", rc);
	} else if (pLockData) {
		/* lock structure can be returned on get */
		__u16 data_offset;
		__u16 data_count;
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) < sizeof(*parm_data)) {
			rc = -EIO;      /* bad smb */
			goto plk_err_exit;
		}
		data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
		data_count  = le16_to_cpu(pSMBr->t2.DataCount);
		if (data_count < sizeof(struct cifs_posix_lock)) {
			rc = -EIO;
			goto plk_err_exit;
		}
		parm_data = (struct cifs_posix_lock *)
			((char *)&pSMBr->hdr.Protocol + data_offset);
		if (parm_data->lock_type == cpu_to_le16(CIFS_UNLCK))
			pLockData->c.flc_type = F_UNLCK;
		else {
			if (parm_data->lock_type ==
					cpu_to_le16(CIFS_RDLCK))
				pLockData->c.flc_type = F_RDLCK;
			else if (parm_data->lock_type ==
					cpu_to_le16(CIFS_WRLCK))
				pLockData->c.flc_type = F_WRLCK;

			pLockData->fl_start = le64_to_cpu(parm_data->start);
			pLockData->fl_end = pLockData->fl_start +
				(le64_to_cpu(parm_data->length) ?
				 le64_to_cpu(parm_data->length) - 1 : 0);
			pLockData->c.flc_pid = -le32_to_cpu(parm_data->pid);
		}
	}

plk_err_exit:
	free_rsp_buf(resp_buf_type, rsp_iov.iov_base);

	/* Note: On -EAGAIN error only caller can retry on handle based calls
	   since file handle passed in no longer valid */

	return rc;
}


int
CIFSSMBClose(const unsigned int xid, struct cifs_tcon *tcon, int smb_file_id)
{
	int rc = 0;
	CLOSE_REQ *pSMB = NULL;
	cifs_dbg(FYI, "In CIFSSMBClose\n");

/* do not retry on dead session on close */
	rc = small_smb_init(SMB_COM_CLOSE, 3, tcon, (void **) &pSMB);
	if (rc == -EAGAIN)
		return 0;
	if (rc)
		return rc;

	pSMB->FileID = (__u16) smb_file_id;
	pSMB->LastWriteTime = 0xFFFFFFFF;
	pSMB->ByteCount = 0;
	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) pSMB, 0);
	cifs_small_buf_release(pSMB);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_closes);
	if (rc) {
		if (rc != -EINTR) {
			/* EINTR is expected when user ctl-c to kill app */
			cifs_dbg(VFS, "Send error in Close = %d\n", rc);
		}
	}

	/* Since session is dead, file will be closed on server already */
	if (rc == -EAGAIN)
		rc = 0;

	return rc;
}

int
CIFSSMBFlush(const unsigned int xid, struct cifs_tcon *tcon, int smb_file_id)
{
	int rc = 0;
	FLUSH_REQ *pSMB = NULL;
	cifs_dbg(FYI, "In CIFSSMBFlush\n");

	rc = small_smb_init(SMB_COM_FLUSH, 1, tcon, (void **) &pSMB);
	if (rc)
		return rc;

	pSMB->FileID = (__u16) smb_file_id;
	pSMB->ByteCount = 0;
	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) pSMB, 0);
	cifs_small_buf_release(pSMB);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_flushes);
	if (rc)
		cifs_dbg(VFS, "Send error in Flush = %d\n", rc);

	return rc;
}

int CIFSSMBRename(const unsigned int xid, struct cifs_tcon *tcon,
		  struct dentry *source_dentry,
		  const char *from_name, const char *to_name,
		  struct cifs_sb_info *cifs_sb)
{
	int rc = 0;
	RENAME_REQ *pSMB = NULL;
	RENAME_RSP *pSMBr = NULL;
	int bytes_returned;
	int name_len, name_len2;
	__u16 count;
	int remap = cifs_remap(cifs_sb);

	cifs_dbg(FYI, "In CIFSSMBRename\n");
renameRetry:
	rc = smb_init(SMB_COM_RENAME, 1, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	pSMB->BufferFormat = 0x04;
	pSMB->SearchAttributes =
	    cpu_to_le16(ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM |
			ATTR_DIRECTORY);

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len = cifsConvertToUTF16((__le16 *) pSMB->OldFileName,
					      from_name, PATH_MAX,
					      cifs_sb->local_nls, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
		pSMB->OldFileName[name_len] = 0x04;	/* pad */
	/* protocol requires ASCII signature byte on Unicode string */
		pSMB->OldFileName[name_len + 1] = 0x00;
		name_len2 =
		    cifsConvertToUTF16((__le16 *)&pSMB->OldFileName[name_len+2],
				       to_name, PATH_MAX, cifs_sb->local_nls,
				       remap);
		name_len2 += 1 /* trailing null */  + 1 /* Signature word */ ;
		name_len2 *= 2;	/* convert to bytes */
	} else {
		name_len = copy_path_name(pSMB->OldFileName, from_name);
		name_len2 = copy_path_name(pSMB->OldFileName+name_len+1, to_name);
		pSMB->OldFileName[name_len] = 0x04;  /* 2nd buffer format */
		name_len2++;	/* signature byte */
	}

	count = 1 /* 1st signature byte */  + name_len + name_len2;
	inc_rfc1001_len(pSMB, count);
	pSMB->ByteCount = cpu_to_le16(count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_renames);
	if (rc)
		cifs_dbg(FYI, "Send error in rename = %d\n", rc);

	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto renameRetry;

	return rc;
}

int CIFSSMBRenameOpenFile(const unsigned int xid, struct cifs_tcon *pTcon,
		int netfid, const char *target_name,
		const struct nls_table *nls_codepage, int remap)
{
	struct smb_com_transaction2_sfi_req *pSMB  = NULL;
	struct smb_com_transaction2_sfi_rsp *pSMBr = NULL;
	struct set_file_rename *rename_info;
	char *data_offset;
	char dummy_string[30];
	int rc = 0;
	int bytes_returned = 0;
	int len_of_str;
	__u16 params, param_offset, offset, count, byte_count;

	cifs_dbg(FYI, "Rename to File by handle\n");
	rc = smb_init(SMB_COM_TRANSACTION2, 15, pTcon, (void **) &pSMB,
			(void **) &pSMBr);
	if (rc)
		return rc;

	params = 6;
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_sfi_req, Fid) - 4;
	offset = param_offset + params;

	/* SMB offsets are from the beginning of SMB which is 4 bytes in, after RFC1001 field */
	data_offset = (char *)(pSMB) + offset + 4;
	rename_info = (struct set_file_rename *) data_offset;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	pSMB->MaxDataCount = cpu_to_le16(1000); /* BB find max SMB from sess */
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_FILE_INFORMATION);
	byte_count = 3 /* pad */  + params;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	/* construct random name ".cifs_tmp<inodenum><mid>" */
	rename_info->overwrite = cpu_to_le32(1);
	rename_info->root_fid  = 0;
	/* unicode only call */
	if (target_name == NULL) {
		sprintf(dummy_string, "cifs%x", pSMB->hdr.Mid);
		len_of_str =
			cifsConvertToUTF16((__le16 *)rename_info->target_name,
					dummy_string, 24, nls_codepage, remap);
	} else {
		len_of_str =
			cifsConvertToUTF16((__le16 *)rename_info->target_name,
					target_name, PATH_MAX, nls_codepage,
					remap);
	}
	rename_info->target_name_len = cpu_to_le32(2 * len_of_str);
	count = sizeof(struct set_file_rename) + (2 * len_of_str);
	byte_count += count;
	pSMB->DataCount = cpu_to_le16(count);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->Fid = netfid;
	pSMB->InformationLevel =
		cpu_to_le16(SMB_SET_FILE_RENAME_INFORMATION);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, pTcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&pTcon->stats.cifs_stats.num_t2renames);
	if (rc)
		cifs_dbg(FYI, "Send error in Rename (by file handle) = %d\n",
			 rc);

	cifs_buf_release(pSMB);

	/* Note: On -EAGAIN error only caller can retry on handle based calls
		since file handle passed in no longer valid */

	return rc;
}

int
CIFSUnixCreateSymLink(const unsigned int xid, struct cifs_tcon *tcon,
		      const char *fromName, const char *toName,
		      const struct nls_table *nls_codepage, int remap)
{
	TRANSACTION2_SPI_REQ *pSMB = NULL;
	TRANSACTION2_SPI_RSP *pSMBr = NULL;
	char *data_offset;
	int name_len;
	int name_len_target;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, param_offset, offset, byte_count;

	cifs_dbg(FYI, "In Symlink Unix style\n");
createSymLinkRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, fromName,
				/* find define for this maxpathcomponent */
					PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;

	} else {
		name_len = copy_path_name(pSMB->FileName, fromName);
	}
	params = 6 + name_len;
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;

	/* SMB offsets are from the beginning of SMB which is 4 bytes in, after RFC1001 field */
	data_offset = (char *)pSMB + offset + 4;
	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len_target =
		    cifsConvertToUTF16((__le16 *) data_offset, toName,
				/* find define for this maxpathcomponent */
					PATH_MAX, nls_codepage, remap);
		name_len_target++;	/* trailing null */
		name_len_target *= 2;
	} else {
		name_len_target = copy_path_name(data_offset, toName);
	}

	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max on data count below from sess */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	byte_count = 3 /* pad */  + params + name_len_target;
	pSMB->DataCount = cpu_to_le16(name_len_target);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_UNIX_LINK);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_symlinks);
	if (rc)
		cifs_dbg(FYI, "Send error in SetPathInfo create symlink = %d\n",
			 rc);

	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto createSymLinkRetry;

	return rc;
}

int
CIFSUnixCreateHardLink(const unsigned int xid, struct cifs_tcon *tcon,
		       const char *fromName, const char *toName,
		       const struct nls_table *nls_codepage, int remap)
{
	TRANSACTION2_SPI_REQ *pSMB = NULL;
	TRANSACTION2_SPI_RSP *pSMBr = NULL;
	char *data_offset;
	int name_len;
	int name_len_target;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, param_offset, offset, byte_count;

	cifs_dbg(FYI, "In Create Hard link Unix style\n");
createHardLinkRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len = cifsConvertToUTF16((__le16 *) pSMB->FileName, toName,
					      PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;

	} else {
		name_len = copy_path_name(pSMB->FileName, toName);
	}
	params = 6 + name_len;
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;

	/* SMB offsets are from the beginning of SMB which is 4 bytes in, after RFC1001 field */
	data_offset = (char *)pSMB + offset + 4;
	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len_target =
		    cifsConvertToUTF16((__le16 *) data_offset, fromName,
				       PATH_MAX, nls_codepage, remap);
		name_len_target++;	/* trailing null */
		name_len_target *= 2;
	} else {
		name_len_target = copy_path_name(data_offset, fromName);
	}

	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max on data count below from sess*/
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	byte_count = 3 /* pad */  + params + name_len_target;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->DataCount = cpu_to_le16(name_len_target);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_UNIX_HLINK);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_hardlinks);
	if (rc)
		cifs_dbg(FYI, "Send error in SetPathInfo (hard link) = %d\n",
			 rc);

	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto createHardLinkRetry;

	return rc;
}

int CIFSCreateHardLink(const unsigned int xid,
		       struct cifs_tcon *tcon,
		       struct dentry *source_dentry,
		       const char *from_name, const char *to_name,
		       struct cifs_sb_info *cifs_sb)
{
	int rc = 0;
	NT_RENAME_REQ *pSMB = NULL;
	RENAME_RSP *pSMBr = NULL;
	int bytes_returned;
	int name_len, name_len2;
	__u16 count;
	int remap = cifs_remap(cifs_sb);

	cifs_dbg(FYI, "In CIFSCreateHardLink\n");
winCreateHardLinkRetry:

	rc = smb_init(SMB_COM_NT_RENAME, 4, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	pSMB->SearchAttributes =
	    cpu_to_le16(ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM |
			ATTR_DIRECTORY);
	pSMB->Flags = cpu_to_le16(CREATE_HARD_LINK);
	pSMB->ClusterCount = 0;

	pSMB->BufferFormat = 0x04;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->OldFileName, from_name,
				       PATH_MAX, cifs_sb->local_nls, remap);
		name_len++;	/* trailing null */
		name_len *= 2;

		/* protocol specifies ASCII buffer format (0x04) for unicode */
		pSMB->OldFileName[name_len] = 0x04;
		pSMB->OldFileName[name_len + 1] = 0x00; /* pad */
		name_len2 =
		    cifsConvertToUTF16((__le16 *)&pSMB->OldFileName[name_len+2],
				       to_name, PATH_MAX, cifs_sb->local_nls,
				       remap);
		name_len2 += 1 /* trailing null */  + 1 /* Signature word */ ;
		name_len2 *= 2;	/* convert to bytes */
	} else {
		name_len = copy_path_name(pSMB->OldFileName, from_name);
		pSMB->OldFileName[name_len] = 0x04;	/* 2nd buffer format */
		name_len2 = copy_path_name(pSMB->OldFileName+name_len+1, to_name);
		name_len2++;	/* signature byte */
	}

	count = 1 /* string type byte */  + name_len + name_len2;
	inc_rfc1001_len(pSMB, count);
	pSMB->ByteCount = cpu_to_le16(count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_hardlinks);
	if (rc)
		cifs_dbg(FYI, "Send error in hard link (NT rename) = %d\n", rc);

	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto winCreateHardLinkRetry;

	return rc;
}

int
CIFSSMBUnixQuerySymLink(const unsigned int xid, struct cifs_tcon *tcon,
			const unsigned char *searchName, char **symlinkinfo,
			const struct nls_table *nls_codepage, int remap)
{
/* SMB_QUERY_FILE_UNIX_LINK */
	TRANSACTION2_QPI_REQ *pSMB = NULL;
	TRANSACTION2_QPI_RSP *pSMBr = NULL;
	int rc = 0;
	int bytes_returned;
	int name_len;
	__u16 params, byte_count;
	char *data_start;

	cifs_dbg(FYI, "In QPathSymLinkInfo (Unix) for path %s\n", searchName);

querySymLinkRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
			cifsConvertToUTF16((__le16 *) pSMB->FileName,
					   searchName, PATH_MAX, nls_codepage,
					   remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, searchName);
	}

	params = 2 /* level */  + 4 /* rsrvd */  + name_len /* incl null */ ;
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	pSMB->MaxDataCount = cpu_to_le16(CIFSMaxBufSize);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
	struct smb_com_transaction2_qpi_req, InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_PATH_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FILE_UNIX_LINK);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QuerySymLinkInfo = %d\n", rc);
	} else {
		/* decode response */

		rc = validate_t2((struct smb_t2_rsp *)pSMBr);
		/* BB also check enough total bytes returned */
		if (rc || get_bcc(&pSMBr->hdr) < 2)
			rc = -EIO;
		else {
			bool is_unicode;
			u16 count = le16_to_cpu(pSMBr->t2.DataCount);

			data_start = ((char *) &pSMBr->hdr.Protocol) +
					   le16_to_cpu(pSMBr->t2.DataOffset);

			if (pSMBr->hdr.Flags2 & SMBFLG2_UNICODE)
				is_unicode = true;
			else
				is_unicode = false;

			/* BB FIXME investigate remapping reserved chars here */
			*symlinkinfo = cifs_strndup_from_utf16(data_start,
					count, is_unicode, nls_codepage);
			if (!*symlinkinfo)
				rc = -ENOMEM;
		}
	}
	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto querySymLinkRetry;
	return rc;
}

int cifs_query_reparse_point(const unsigned int xid,
			     struct cifs_tcon *tcon,
			     struct cifs_sb_info *cifs_sb,
			     const char *full_path,
			     u32 *tag, struct kvec *rsp,
			     int *rsp_buftype)
{
	struct reparse_data_buffer *buf;
	struct cifs_open_parms oparms;
	TRANSACT_IOCTL_REQ *io_req = NULL;
	TRANSACT_IOCTL_RSP *io_rsp = NULL;
	struct cifs_fid fid;
	__u32 data_offset, data_count, len;
	__u8 *start, *end;
	int io_rsp_len;
	int oplock = 0;
	int rc;

	cifs_tcon_dbg(FYI, "%s: path=%s\n", __func__, full_path);

	if (cap_unix(tcon->ses))
		return -EOPNOTSUPP;

	if (!(le32_to_cpu(tcon->fsAttrInfo.Attributes) & FILE_SUPPORTS_REPARSE_POINTS))
		return -EOPNOTSUPP;

	oparms = (struct cifs_open_parms) {
		.tcon = tcon,
		.cifs_sb = cifs_sb,
		.desired_access = FILE_READ_ATTRIBUTES,
		.create_options = cifs_create_options(cifs_sb,
						      OPEN_REPARSE_POINT),
		.disposition = FILE_OPEN,
		.path = full_path,
		.fid = &fid,
	};

	rc = CIFS_open(xid, &oparms, &oplock, NULL);
	if (rc)
		return rc;

	rc = smb_init(SMB_COM_NT_TRANSACT, 23, tcon,
		      (void **)&io_req, (void **)&io_rsp);
	if (rc)
		goto error;

	io_req->TotalParameterCount = 0;
	io_req->TotalDataCount = 0;
	io_req->MaxParameterCount = cpu_to_le32(0);
	/* BB find exact data count max from sess structure BB */
	io_req->MaxDataCount = cpu_to_le32(CIFSMaxBufSize & 0xFFFFFF00);
	io_req->MaxSetupCount = 1;
	io_req->Reserved = 0;
	io_req->ParameterOffset = 0;
	io_req->DataCount = 0;
	io_req->DataOffset = 0;
	io_req->SetupCount = 4;
	io_req->SubCommand = cpu_to_le16(NT_TRANSACT_IOCTL);
	io_req->ParameterCount = io_req->TotalParameterCount;
	io_req->FunctionCode = cpu_to_le32(FSCTL_GET_REPARSE_POINT);
	io_req->IsFsctl = 1;
	io_req->IsRootFlag = 0;
	io_req->Fid = fid.netfid;
	io_req->ByteCount = 0;

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *)io_req,
			 (struct smb_hdr *)io_rsp, &io_rsp_len, 0);
	if (rc)
		goto error;

	data_offset = le32_to_cpu(io_rsp->DataOffset);
	data_count = le32_to_cpu(io_rsp->DataCount);
	if (get_bcc(&io_rsp->hdr) < 2 || data_offset > 512 ||
	    !data_count || data_count > 2048) {
		rc = -EIO;
		goto error;
	}

	/* SetupCount must be 1, otherwise offset to ByteCount is incorrect. */
	if (io_rsp->SetupCount != 1) {
		rc = -EIO;
		goto error;
	}

	/*
	 * ReturnedDataLen is output length of executed IOCTL.
	 * DataCount is output length transferred over network.
	 * Check that we have full FSCTL_GET_REPARSE_POINT buffer.
	 */
	if (data_count != le16_to_cpu(io_rsp->ReturnedDataLen)) {
		rc = -EIO;
		goto error;
	}

	end = 2 + get_bcc(&io_rsp->hdr) + (__u8 *)&io_rsp->ByteCount;
	start = (__u8 *)&io_rsp->hdr.Protocol + data_offset;
	if (start >= end) {
		rc = -EIO;
		goto error;
	}

	data_count = le16_to_cpu(io_rsp->ByteCount);
	buf = (struct reparse_data_buffer *)start;
	len = sizeof(*buf);
	if (data_count < len ||
	    data_count < le16_to_cpu(buf->ReparseDataLength) + len) {
		rc = -EIO;
		goto error;
	}

	*tag = le32_to_cpu(buf->ReparseTag);
	rsp->iov_base = io_rsp;
	rsp->iov_len = io_rsp_len;
	*rsp_buftype = CIFS_LARGE_BUFFER;
	CIFSSMBClose(xid, tcon, fid.netfid);
	return 0;

error:
	cifs_buf_release(io_req);
	CIFSSMBClose(xid, tcon, fid.netfid);
	return rc;
}

struct inode *cifs_create_reparse_inode(struct cifs_open_info_data *data,
					struct super_block *sb,
					const unsigned int xid,
					struct cifs_tcon *tcon,
					const char *full_path,
					bool directory,
					struct kvec *reparse_iov,
					struct kvec *xattr_iov)
{
	struct cifs_sb_info *cifs_sb = CIFS_SB(sb);
	struct cifs_open_parms oparms;
	TRANSACT_IOCTL_REQ *io_req;
	struct inode *new = NULL;
	struct kvec in_iov[2];
	struct kvec out_iov;
	struct cifs_fid fid;
	int io_req_len;
	int oplock = 0;
	int buf_type = 0;
	int rc;

	cifs_tcon_dbg(FYI, "%s: path=%s\n", __func__, full_path);

	/*
	 * If server filesystem does not support reparse points then do not
	 * attempt to create reparse point. This will prevent creating unusable
	 * empty object on the server.
	 */
	if (!(le32_to_cpu(tcon->fsAttrInfo.Attributes) & FILE_SUPPORTS_REPARSE_POINTS))
		return ERR_PTR(-EOPNOTSUPP);

#ifndef CONFIG_CIFS_XATTR
	if (xattr_iov)
		return ERR_PTR(-EOPNOTSUPP);
#endif

	oparms = CIFS_OPARMS(cifs_sb, tcon, full_path,
			     FILE_READ_ATTRIBUTES | FILE_WRITE_DATA | FILE_WRITE_EA,
			     FILE_CREATE,
			     (directory ? CREATE_NOT_FILE : CREATE_NOT_DIR) | OPEN_REPARSE_POINT,
			     ACL_NO_MODE);
	oparms.fid = &fid;

	rc = CIFS_open(xid, &oparms, &oplock, NULL);
	if (rc)
		return ERR_PTR(rc);

#ifdef CONFIG_CIFS_XATTR
	if (xattr_iov) {
		struct smb2_file_full_ea_info *ea;

		ea = &((struct smb2_create_ea_ctx *)xattr_iov->iov_base)->ea;
		while (1) {
			rc = CIFSSMBSetEA(xid,
					  tcon,
					  full_path,
					  &ea->ea_data[0],
					  &ea->ea_data[ea->ea_name_length+1],
					  le16_to_cpu(ea->ea_value_length),
					  cifs_sb->local_nls,
					  cifs_sb);
			if (rc)
				goto out_close;
			if (le32_to_cpu(ea->next_entry_offset) == 0)
				break;
			ea = (struct smb2_file_full_ea_info *)((u8 *)ea +
				le32_to_cpu(ea->next_entry_offset));
		}
	}
#endif

	rc = smb_init(SMB_COM_NT_TRANSACT, 23, tcon, (void **)&io_req, NULL);
	if (rc)
		goto out_close;

	inc_rfc1001_len(io_req, sizeof(io_req->Pad));

	io_req_len = be32_to_cpu(io_req->hdr.smb_buf_length) + sizeof(io_req->hdr.smb_buf_length);

	/* NT IOCTL response contains one-word long output setup buffer with size of output data. */
	io_req->MaxSetupCount = 1;
	/* NT IOCTL response does not contain output parameters. */
	io_req->MaxParameterCount = cpu_to_le32(0);
	/* FSCTL_SET_REPARSE_POINT response contains empty output data. */
	io_req->MaxDataCount = cpu_to_le32(0);

	io_req->TotalParameterCount = cpu_to_le32(0);
	io_req->TotalDataCount = cpu_to_le32(reparse_iov->iov_len);
	io_req->ParameterCount = io_req->TotalParameterCount;
	io_req->ParameterOffset = cpu_to_le32(0);
	io_req->DataCount = io_req->TotalDataCount;
	io_req->DataOffset = cpu_to_le32(offsetof(typeof(*io_req), Data) -
					 sizeof(io_req->hdr.smb_buf_length));
	io_req->SetupCount = 4;
	io_req->SubCommand = cpu_to_le16(NT_TRANSACT_IOCTL);
	io_req->FunctionCode = cpu_to_le32(FSCTL_SET_REPARSE_POINT);
	io_req->Fid = fid.netfid;
	io_req->IsFsctl = 1;
	io_req->IsRootFlag = 0;
	io_req->ByteCount = cpu_to_le16(le32_to_cpu(io_req->DataCount) + sizeof(io_req->Pad));

	inc_rfc1001_len(io_req, reparse_iov->iov_len);

	in_iov[0].iov_base = (char *)io_req;
	in_iov[0].iov_len = io_req_len;
	in_iov[1] = *reparse_iov;
	rc = SendReceive2(xid, tcon->ses, in_iov, ARRAY_SIZE(in_iov), &buf_type,
			  CIFS_NO_RSP_BUF, &out_iov);

	cifs_buf_release(io_req);

	if (!rc)
		rc = cifs_get_inode_info(&new, full_path, data, sb, xid, NULL);

out_close:
	CIFSSMBClose(xid, tcon, fid.netfid);

	/*
	 * If CREATE was successful but FSCTL_SET_REPARSE_POINT failed then
	 * remove the intermediate object created by CREATE. Otherwise
	 * empty object stay on the server when reparse call failed.
	 */
	if (rc)
		CIFSSMBDelFile(xid, tcon, full_path, cifs_sb, NULL);

	return rc ? ERR_PTR(rc) : new;
}

int
CIFSSMB_set_compression(const unsigned int xid, struct cifs_tcon *tcon,
		    __u16 fid)
{
	int rc = 0;
	int bytes_returned;
	struct smb_com_transaction_compr_ioctl_req *pSMB;
	struct smb_com_transaction_ioctl_rsp *pSMBr;

	cifs_dbg(FYI, "Set compression for %u\n", fid);
	rc = smb_init(SMB_COM_NT_TRANSACT, 23, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	pSMB->compression_state = cpu_to_le16(COMPRESSION_FORMAT_DEFAULT);

	pSMB->TotalParameterCount = 0;
	pSMB->TotalDataCount = cpu_to_le32(2);
	pSMB->MaxParameterCount = 0;
	pSMB->MaxDataCount = 0;
	pSMB->MaxSetupCount = 4;
	pSMB->Reserved = 0;
	pSMB->ParameterOffset = 0;
	pSMB->DataCount = cpu_to_le32(2);
	pSMB->DataOffset =
		cpu_to_le32(offsetof(struct smb_com_transaction_compr_ioctl_req,
				compression_state) - 4);  /* 84 */
	pSMB->SetupCount = 4;
	pSMB->SubCommand = cpu_to_le16(NT_TRANSACT_IOCTL);
	pSMB->ParameterCount = 0;
	pSMB->FunctionCode = cpu_to_le32(FSCTL_SET_COMPRESSION);
	pSMB->IsFsctl = 1; /* FSCTL */
	pSMB->IsRootFlag = 0;
	pSMB->Fid = fid; /* file handle always le */
	/* 3 byte pad, followed by 2 byte compress state */
	pSMB->ByteCount = cpu_to_le16(5);
	inc_rfc1001_len(pSMB, 5);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc)
		cifs_dbg(FYI, "Send error in SetCompression = %d\n", rc);

	cifs_buf_release(pSMB);

	/*
	 * Note: On -EAGAIN error only caller can retry on handle based calls
	 * since file handle passed in no longer valid.
	 */
	return rc;
}


#ifdef CONFIG_CIFS_POSIX

#ifdef CONFIG_FS_POSIX_ACL
/**
 * cifs_init_posix_acl - convert ACL from cifs to POSIX ACL format
 * @ace: POSIX ACL entry to store converted ACL into
 * @cifs_ace: ACL in cifs format
 *
 * Convert an Access Control Entry from wire format to local POSIX xattr
 * format.
 *
 * Note that the @cifs_uid member is used to store both {g,u}id_t.
 */
static void cifs_init_posix_acl(struct posix_acl_entry *ace,
				struct cifs_posix_ace *cifs_ace)
{
	/* u8 cifs fields do not need le conversion */
	ace->e_perm = cifs_ace->cifs_e_perm;
	ace->e_tag = cifs_ace->cifs_e_tag;

	switch (ace->e_tag) {
	case ACL_USER:
		ace->e_uid = make_kuid(&init_user_ns,
				       le64_to_cpu(cifs_ace->cifs_uid));
		break;
	case ACL_GROUP:
		ace->e_gid = make_kgid(&init_user_ns,
				       le64_to_cpu(cifs_ace->cifs_uid));
		break;
	}
	return;
}

/**
 * cifs_to_posix_acl - copy cifs ACL format to POSIX ACL format
 * @acl: ACLs returned in POSIX ACL format
 * @src: ACLs in cifs format
 * @acl_type: type of POSIX ACL requested
 * @size_of_data_area: size of SMB we got
 *
 * This function converts ACLs from cifs format to POSIX ACL format.
 * If @acl is NULL then the size of the buffer required to store POSIX ACLs in
 * their uapi format is returned.
 */
static int cifs_to_posix_acl(struct posix_acl **acl, char *src,
			     const int acl_type, const int size_of_data_area)
{
	int size =  0;
	__u16 count;
	struct cifs_posix_ace *pACE;
	struct cifs_posix_acl *cifs_acl = (struct cifs_posix_acl *)src;
	struct posix_acl *kacl = NULL;
	struct posix_acl_entry *pa, *pe;

	if (le16_to_cpu(cifs_acl->version) != CIFS_ACL_VERSION)
		return -EOPNOTSUPP;

	if (acl_type == ACL_TYPE_ACCESS) {
		count = le16_to_cpu(cifs_acl->access_entry_count);
		pACE = &cifs_acl->ace_array[0];
		size = sizeof(struct cifs_posix_acl);
		size += sizeof(struct cifs_posix_ace) * count;
		/* check if we would go beyond end of SMB */
		if (size_of_data_area < size) {
			cifs_dbg(FYI, "bad CIFS POSIX ACL size %d vs. %d\n",
				 size_of_data_area, size);
			return -EINVAL;
		}
	} else if (acl_type == ACL_TYPE_DEFAULT) {
		count = le16_to_cpu(cifs_acl->access_entry_count);
		size = sizeof(struct cifs_posix_acl);
		size += sizeof(struct cifs_posix_ace) * count;
		/* skip past access ACEs to get to default ACEs */
		pACE = &cifs_acl->ace_array[count];
		count = le16_to_cpu(cifs_acl->default_entry_count);
		size += sizeof(struct cifs_posix_ace) * count;
		/* check if we would go beyond end of SMB */
		if (size_of_data_area < size)
			return -EINVAL;
	} else {
		/* illegal type */
		return -EINVAL;
	}

	/* Allocate number of POSIX ACLs to store in VFS format. */
	kacl = posix_acl_alloc(count, GFP_NOFS);
	if (!kacl)
		return -ENOMEM;

	FOREACH_ACL_ENTRY(pa, kacl, pe) {
		cifs_init_posix_acl(pa, pACE);
		pACE++;
	}

	*acl = kacl;
	return 0;
}

/**
 * cifs_init_ace - convert ACL entry from POSIX ACL to cifs format
 * @cifs_ace: the cifs ACL entry to store into
 * @local_ace: the POSIX ACL entry to convert
 */
static void cifs_init_ace(struct cifs_posix_ace *cifs_ace,
			  const struct posix_acl_entry *local_ace)
{
	cifs_ace->cifs_e_perm = local_ace->e_perm;
	cifs_ace->cifs_e_tag =  local_ace->e_tag;

	switch (local_ace->e_tag) {
	case ACL_USER:
		cifs_ace->cifs_uid =
			cpu_to_le64(from_kuid(&init_user_ns, local_ace->e_uid));
		break;
	case ACL_GROUP:
		cifs_ace->cifs_uid =
			cpu_to_le64(from_kgid(&init_user_ns, local_ace->e_gid));
		break;
	default:
		cifs_ace->cifs_uid = cpu_to_le64(-1);
	}
}

/**
 * posix_acl_to_cifs - convert ACLs from POSIX ACL to cifs format
 * @parm_data: ACLs in cifs format to convert to
 * @acl: ACLs in POSIX ACL format to convert from
 * @acl_type: the type of POSIX ACLs stored in @acl
 *
 * Return: the number cifs ACL entries after conversion
 */
static __u16 posix_acl_to_cifs(char *parm_data, const struct posix_acl *acl,
			       const int acl_type)
{
	__u16 rc = 0;
	struct cifs_posix_acl *cifs_acl = (struct cifs_posix_acl *)parm_data;
	const struct posix_acl_entry *pa, *pe;
	int count;
	int i = 0;

	if ((acl == NULL) || (cifs_acl == NULL))
		return 0;

	count = acl->a_count;
	cifs_dbg(FYI, "setting acl with %d entries\n", count);

	/*
	 * Note that the uapi POSIX ACL version is verified by the VFS and is
	 * independent of the cifs ACL version. Changing the POSIX ACL version
	 * is a uapi change and if it's changed we will pass down the POSIX ACL
	 * version in struct posix_acl from the VFS. For now there's really
	 * only one that all filesystems know how to deal with.
	 */
	cifs_acl->version = cpu_to_le16(1);
	if (acl_type == ACL_TYPE_ACCESS) {
		cifs_acl->access_entry_count = cpu_to_le16(count);
		cifs_acl->default_entry_count = cpu_to_le16(0xFFFF);
	} else if (acl_type == ACL_TYPE_DEFAULT) {
		cifs_acl->default_entry_count = cpu_to_le16(count);
		cifs_acl->access_entry_count = cpu_to_le16(0xFFFF);
	} else {
		cifs_dbg(FYI, "unknown ACL type %d\n", acl_type);
		return 0;
	}
	FOREACH_ACL_ENTRY(pa, acl, pe) {
		cifs_init_ace(&cifs_acl->ace_array[i++], pa);
	}
	if (rc == 0) {
		rc = (__u16)(count * sizeof(struct cifs_posix_ace));
		rc += sizeof(struct cifs_posix_acl);
		/* BB add check to make sure ACL does not overflow SMB */
	}
	return rc;
}

int cifs_do_get_acl(const unsigned int xid, struct cifs_tcon *tcon,
		    const unsigned char *searchName, struct posix_acl **acl,
		    const int acl_type, const struct nls_table *nls_codepage,
		    int remap)
{
/* SMB_QUERY_POSIX_ACL */
	TRANSACTION2_QPI_REQ *pSMB = NULL;
	TRANSACTION2_QPI_RSP *pSMBr = NULL;
	int rc = 0;
	int bytes_returned;
	int name_len;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In GetPosixACL (Unix) for path %s\n", searchName);

queryAclRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		(void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
			cifsConvertToUTF16((__le16 *) pSMB->FileName,
					   searchName, PATH_MAX, nls_codepage,
					   remap);
		name_len++;     /* trailing null */
		name_len *= 2;
		pSMB->FileName[name_len] = 0;
		pSMB->FileName[name_len+1] = 0;
	} else {
		name_len = copy_path_name(pSMB->FileName, searchName);
	}

	params = 2 /* level */  + 4 /* rsrvd */  + name_len /* incl null */ ;
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max data count below from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(4000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	pSMB->ParameterOffset = cpu_to_le16(
		offsetof(struct smb_com_transaction2_qpi_req,
			 InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_PATH_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_POSIX_ACL);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
		(struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_acl_get);
	if (rc) {
		cifs_dbg(FYI, "Send error in Query POSIX ACL = %d\n", rc);
	} else {
		/* decode response */

		rc = validate_t2((struct smb_t2_rsp *)pSMBr);
		/* BB also check enough total bytes returned */
		if (rc || get_bcc(&pSMBr->hdr) < 2)
			rc = -EIO;      /* bad smb */
		else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			__u16 count = le16_to_cpu(pSMBr->t2.DataCount);
			rc = cifs_to_posix_acl(acl,
				(char *)&pSMBr->hdr.Protocol+data_offset,
				acl_type, count);
		}
	}
	cifs_buf_release(pSMB);
	/*
	 * The else branch after SendReceive() doesn't return EAGAIN so if we
	 * allocated @acl in cifs_to_posix_acl() we are guaranteed to return
	 * here and don't leak POSIX ACLs.
	 */
	if (rc == -EAGAIN)
		goto queryAclRetry;
	return rc;
}

int cifs_do_set_acl(const unsigned int xid, struct cifs_tcon *tcon,
		    const unsigned char *fileName, const struct posix_acl *acl,
		    const int acl_type, const struct nls_table *nls_codepage,
		    int remap)
{
	struct smb_com_transaction2_spi_req *pSMB = NULL;
	struct smb_com_transaction2_spi_rsp *pSMBr = NULL;
	char *parm_data;
	int name_len;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, byte_count, data_count, param_offset, offset;

	cifs_dbg(FYI, "In SetPosixACL (Unix) for path %s\n", fileName);
setAclRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;
	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
			cifsConvertToUTF16((__le16 *) pSMB->FileName, fileName,
					   PATH_MAX, nls_codepage, remap);
		name_len++;     /* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, fileName);
	}
	params = 6 + name_len;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find max SMB size from sess */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;
	parm_data = ((char *)pSMB) + sizeof(pSMB->hdr.smb_buf_length) + offset;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);

	/* convert to on the wire format for POSIX ACL */
	data_count = posix_acl_to_cifs(parm_data, acl, acl_type);

	if (data_count == 0) {
		rc = -EOPNOTSUPP;
		goto setACLerrorExit;
	}
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	pSMB->InformationLevel = cpu_to_le16(SMB_SET_POSIX_ACL);
	byte_count = 3 /* pad */  + params + data_count;
	pSMB->DataCount = cpu_to_le16(data_count);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc)
		cifs_dbg(FYI, "Set POSIX ACL returned %d\n", rc);

setACLerrorExit:
	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto setAclRetry;
	return rc;
}
#else
int cifs_do_get_acl(const unsigned int xid, struct cifs_tcon *tcon,
		    const unsigned char *searchName, struct posix_acl **acl,
		    const int acl_type, const struct nls_table *nls_codepage,
		    int remap)
{
	return -EOPNOTSUPP;
}

int cifs_do_set_acl(const unsigned int xid, struct cifs_tcon *tcon,
		    const unsigned char *fileName, const struct posix_acl *acl,
		    const int acl_type, const struct nls_table *nls_codepage,
		    int remap)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_FS_POSIX_ACL */

int
CIFSGetExtAttr(const unsigned int xid, struct cifs_tcon *tcon,
	       const int netfid, __u64 *pExtAttrBits, __u64 *pMask)
{
	int rc = 0;
	struct smb_t2_qfi_req *pSMB = NULL;
	struct smb_t2_qfi_rsp *pSMBr = NULL;
	int bytes_returned;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In GetExtAttr\n");
	if (tcon == NULL)
		return -ENODEV;

GetExtAttrRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	params = 2 /* level */ + 2 /* fid */;
	pSMB->t2.TotalDataCount = 0;
	pSMB->t2.MaxParameterCount = cpu_to_le16(4);
	/* BB find exact max data count below from sess structure BB */
	pSMB->t2.MaxDataCount = cpu_to_le16(4000);
	pSMB->t2.MaxSetupCount = 0;
	pSMB->t2.Reserved = 0;
	pSMB->t2.Flags = 0;
	pSMB->t2.Timeout = 0;
	pSMB->t2.Reserved2 = 0;
	pSMB->t2.ParameterOffset = cpu_to_le16(offsetof(struct smb_t2_qfi_req,
					       Fid) - 4);
	pSMB->t2.DataCount = 0;
	pSMB->t2.DataOffset = 0;
	pSMB->t2.SetupCount = 1;
	pSMB->t2.Reserved3 = 0;
	pSMB->t2.SubCommand = cpu_to_le16(TRANS2_QUERY_FILE_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->t2.TotalParameterCount = cpu_to_le16(params);
	pSMB->t2.ParameterCount = pSMB->t2.TotalParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_ATTR_FLAGS);
	pSMB->Pad = 0;
	pSMB->Fid = netfid;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->t2.ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "error %d in GetExtAttr\n", rc);
	} else {
		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);
		/* BB also check enough total bytes returned */
		if (rc || get_bcc(&pSMBr->hdr) < 2)
			/* If rc should we check for EOPNOSUPP and
			   disable the srvino flag? or in caller? */
			rc = -EIO;      /* bad smb */
		else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			__u16 count = le16_to_cpu(pSMBr->t2.DataCount);
			struct file_chattr_info *pfinfo;

			if (count != 16) {
				cifs_dbg(FYI, "Invalid size ret in GetExtAttr\n");
				rc = -EIO;
				goto GetExtAttrOut;
			}
			pfinfo = (struct file_chattr_info *)
				 (data_offset + (char *) &pSMBr->hdr.Protocol);
			*pExtAttrBits = le64_to_cpu(pfinfo->mode);
			*pMask = le64_to_cpu(pfinfo->mask);
		}
	}
GetExtAttrOut:
	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto GetExtAttrRetry;
	return rc;
}

#endif /* CONFIG_POSIX */

/*
 * Initialize NT TRANSACT SMB into small smb request buffer.  This assumes that
 * all NT TRANSACTS that we init here have total parm and data under about 400
 * bytes (to fit in small cifs buffer size), which is the case so far, it
 * easily fits. NB: Setup words themselves and ByteCount MaxSetupCount (size of
 * returned setup area) and MaxParameterCount (returned parms size) must be set
 * by caller
 */
static int
smb_init_nttransact(const __u16 sub_command, const int setup_count,
		   const int parm_len, struct cifs_tcon *tcon,
		   void **ret_buf)
{
	int rc;
	__u32 temp_offset;
	struct smb_com_ntransact_req *pSMB;

	rc = small_smb_init(SMB_COM_NT_TRANSACT, 19 + setup_count, tcon,
				(void **)&pSMB);
	if (rc)
		return rc;
	*ret_buf = (void *)pSMB;
	pSMB->Reserved = 0;
	pSMB->TotalParameterCount = cpu_to_le32(parm_len);
	pSMB->TotalDataCount  = 0;
	pSMB->MaxDataCount = cpu_to_le32(CIFSMaxBufSize & 0xFFFFFF00);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->DataCount  = pSMB->TotalDataCount;
	temp_offset = offsetof(struct smb_com_ntransact_req, Parms) +
			(setup_count * 2) - 4 /* for rfc1001 length itself */;
	pSMB->ParameterOffset = cpu_to_le32(temp_offset);
	pSMB->DataOffset = cpu_to_le32(temp_offset + parm_len);
	pSMB->SetupCount = setup_count; /* no need to le convert byte fields */
	pSMB->SubCommand = cpu_to_le16(sub_command);
	return 0;
}

static int
validate_ntransact(char *buf, char **ppparm, char **ppdata,
		   __u32 *pparmlen, __u32 *pdatalen)
{
	char *end_of_smb;
	__u32 data_count, data_offset, parm_count, parm_offset;
	struct smb_com_ntransact_rsp *pSMBr;
	u16 bcc;

	*pdatalen = 0;
	*pparmlen = 0;

	if (buf == NULL)
		return -EINVAL;

	pSMBr = (struct smb_com_ntransact_rsp *)buf;

	bcc = get_bcc(&pSMBr->hdr);
	end_of_smb = 2 /* sizeof byte count */ + bcc +
			(char *)&pSMBr->ByteCount;

	data_offset = le32_to_cpu(pSMBr->DataOffset);
	data_count = le32_to_cpu(pSMBr->DataCount);
	parm_offset = le32_to_cpu(pSMBr->ParameterOffset);
	parm_count = le32_to_cpu(pSMBr->ParameterCount);

	*ppparm = (char *)&pSMBr->hdr.Protocol + parm_offset;
	*ppdata = (char *)&pSMBr->hdr.Protocol + data_offset;

	/* should we also check that parm and data areas do not overlap? */
	if (*ppparm > end_of_smb) {
		cifs_dbg(FYI, "parms start after end of smb\n");
		return -EINVAL;
	} else if (parm_count + *ppparm > end_of_smb) {
		cifs_dbg(FYI, "parm end after end of smb\n");
		return -EINVAL;
	} else if (*ppdata > end_of_smb) {
		cifs_dbg(FYI, "data starts after end of smb\n");
		return -EINVAL;
	} else if (data_count + *ppdata > end_of_smb) {
		cifs_dbg(FYI, "data %p + count %d (%p) past smb end %p start %p\n",
			 *ppdata, data_count, (data_count + *ppdata),
			 end_of_smb, pSMBr);
		return -EINVAL;
	} else if (parm_count + data_count > bcc) {
		cifs_dbg(FYI, "parm count and data count larger than SMB\n");
		return -EINVAL;
	}
	*pdatalen = data_count;
	*pparmlen = parm_count;
	return 0;
}

/* Get Security Descriptor (by handle) from remote server for a file or dir */
int
CIFSSMBGetCIFSACL(const unsigned int xid, struct cifs_tcon *tcon, __u16 fid,
		  struct smb_ntsd **acl_inf, __u32 *pbuflen, __u32 info)
{
	int rc = 0;
	int buf_type = 0;
	QUERY_SEC_DESC_REQ *pSMB;
	struct kvec iov[1];
	struct kvec rsp_iov;

	cifs_dbg(FYI, "GetCifsACL\n");

	*pbuflen = 0;
	*acl_inf = NULL;

	rc = smb_init_nttransact(NT_TRANSACT_QUERY_SECURITY_DESC, 0,
			8 /* parm len */, tcon, (void **) &pSMB);
	if (rc)
		return rc;

	pSMB->MaxParameterCount = cpu_to_le32(4);
	/* BB TEST with big acls that might need to be e.g. larger than 16K */
	pSMB->MaxSetupCount = 0;
	pSMB->Fid = fid; /* file handle always le */
	pSMB->AclFlags = cpu_to_le32(info);
	pSMB->ByteCount = cpu_to_le16(11); /* 3 bytes pad + 8 bytes parm */
	inc_rfc1001_len(pSMB, 11);
	iov[0].iov_base = (char *)pSMB;
	iov[0].iov_len = be32_to_cpu(pSMB->hdr.smb_buf_length) + 4;

	rc = SendReceive2(xid, tcon->ses, iov, 1 /* num iovec */, &buf_type,
			  0, &rsp_iov);
	cifs_small_buf_release(pSMB);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_acl_get);
	if (rc) {
		cifs_dbg(FYI, "Send error in QuerySecDesc = %d\n", rc);
	} else {                /* decode response */
		__le32 *parm;
		__u32 parm_len;
		__u32 acl_len;
		struct smb_com_ntransact_rsp *pSMBr;
		char *pdata;

/* validate_nttransact */
		rc = validate_ntransact(rsp_iov.iov_base, (char **)&parm,
					&pdata, &parm_len, pbuflen);
		if (rc)
			goto qsec_out;
		pSMBr = (struct smb_com_ntransact_rsp *)rsp_iov.iov_base;

		cifs_dbg(FYI, "smb %p parm %p data %p\n",
			 pSMBr, parm, *acl_inf);

		if (le32_to_cpu(pSMBr->ParameterCount) != 4) {
			rc = -EIO;      /* bad smb */
			*pbuflen = 0;
			goto qsec_out;
		}

/* BB check that data area is minimum length and as big as acl_len */

		acl_len = le32_to_cpu(*parm);
		if (acl_len != *pbuflen) {
			cifs_dbg(VFS, "acl length %d does not match %d\n",
				 acl_len, *pbuflen);
			if (*pbuflen > acl_len)
				*pbuflen = acl_len;
		}

		/* check if buffer is big enough for the acl
		   header followed by the smallest SID */
		if ((*pbuflen < sizeof(struct smb_ntsd) + 8) ||
		    (*pbuflen >= 64 * 1024)) {
			cifs_dbg(VFS, "bad acl length %d\n", *pbuflen);
			rc = -EINVAL;
			*pbuflen = 0;
		} else {
			*acl_inf = kmemdup(pdata, *pbuflen, GFP_KERNEL);
			if (*acl_inf == NULL) {
				*pbuflen = 0;
				rc = -ENOMEM;
			}
		}
	}
qsec_out:
	free_rsp_buf(buf_type, rsp_iov.iov_base);
	return rc;
}

int
CIFSSMBSetCIFSACL(const unsigned int xid, struct cifs_tcon *tcon, __u16 fid,
			struct smb_ntsd *pntsd, __u32 acllen, int aclflag)
{
	__u16 byte_count, param_count, data_count, param_offset, data_offset;
	int rc = 0;
	int bytes_returned = 0;
	SET_SEC_DESC_REQ *pSMB = NULL;
	void *pSMBr;

setCifsAclRetry:
	rc = smb_init(SMB_COM_NT_TRANSACT, 19, tcon, (void **) &pSMB, &pSMBr);
	if (rc)
		return rc;

	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;

	param_count = 8;
	param_offset = offsetof(struct smb_com_transaction_ssec_req, Fid) - 4;
	data_count = acllen;
	data_offset = param_offset + param_count;
	byte_count = 3 /* pad */  + param_count;

	pSMB->DataCount = cpu_to_le32(data_count);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->MaxParameterCount = cpu_to_le32(4);
	pSMB->MaxDataCount = cpu_to_le32(16384);
	pSMB->ParameterCount = cpu_to_le32(param_count);
	pSMB->ParameterOffset = cpu_to_le32(param_offset);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->DataOffset = cpu_to_le32(data_offset);
	pSMB->SetupCount = 0;
	pSMB->SubCommand = cpu_to_le16(NT_TRANSACT_SET_SECURITY_DESC);
	pSMB->ByteCount = cpu_to_le16(byte_count+data_count);

	pSMB->Fid = fid; /* file handle always le */
	pSMB->Reserved2 = 0;
	pSMB->AclFlags = cpu_to_le32(aclflag);

	if (pntsd && acllen) {
		memcpy((char *)pSMBr + offsetof(struct smb_hdr, Protocol) +
				data_offset, pntsd, acllen);
		inc_rfc1001_len(pSMB, byte_count + data_count);
	} else
		inc_rfc1001_len(pSMB, byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
		(struct smb_hdr *) pSMBr, &bytes_returned, 0);

	cifs_dbg(FYI, "SetCIFSACL bytes_returned: %d, rc: %d\n",
		 bytes_returned, rc);
	if (rc)
		cifs_dbg(FYI, "Set CIFS ACL returned %d\n", rc);
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto setCifsAclRetry;

	return (rc);
}


/* Legacy Query Path Information call for lookup to old servers such
   as Win9x/WinME */
int
SMBQueryInformation(const unsigned int xid, struct cifs_tcon *tcon,
		    const char *search_name, FILE_ALL_INFO *data,
		    const struct nls_table *nls_codepage, int remap)
{
	QUERY_INFORMATION_REQ *pSMB;
	QUERY_INFORMATION_RSP *pSMBr;
	int rc = 0;
	int bytes_returned;
	int name_len;

	cifs_dbg(FYI, "In SMBQPath path %s\n", search_name);
QInfRetry:
	rc = smb_init(SMB_COM_QUERY_INFORMATION, 0, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
			cifsConvertToUTF16((__le16 *) pSMB->FileName,
					   search_name, PATH_MAX, nls_codepage,
					   remap);
		name_len++;     /* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, search_name);
	}
	pSMB->BufferFormat = 0x04;
	name_len++; /* account for buffer type byte */
	inc_rfc1001_len(pSMB, (__u16)name_len);
	pSMB->ByteCount = cpu_to_le16(name_len);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QueryInfo = %d\n", rc);
	} else if (data) {
		struct timespec64 ts;
		__u32 time = le32_to_cpu(pSMBr->last_write_time);

		/* decode response */
		/* BB FIXME - add time zone adjustment BB */
		memset(data, 0, sizeof(FILE_ALL_INFO));
		ts.tv_nsec = 0;
		ts.tv_sec = time;
		/* decode time fields */
		data->ChangeTime = cpu_to_le64(cifs_UnixTimeToNT(ts));
		data->LastWriteTime = data->ChangeTime;
		data->LastAccessTime = 0;
		data->AllocationSize =
			cpu_to_le64(le32_to_cpu(pSMBr->size));
		data->EndOfFile = data->AllocationSize;
		data->Attributes =
			cpu_to_le32(le16_to_cpu(pSMBr->attr));
	} else
		rc = -EIO; /* bad buffer passed in */

	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto QInfRetry;

	return rc;
}

int
CIFSSMBQFileInfo(const unsigned int xid, struct cifs_tcon *tcon,
		 u16 netfid, FILE_ALL_INFO *pFindData)
{
	struct smb_t2_qfi_req *pSMB = NULL;
	struct smb_t2_qfi_rsp *pSMBr = NULL;
	int rc = 0;
	int bytes_returned;
	__u16 params, byte_count;

QFileInfoRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	params = 2 /* level */ + 2 /* fid */;
	pSMB->t2.TotalDataCount = 0;
	pSMB->t2.MaxParameterCount = cpu_to_le16(4);
	/* BB find exact max data count below from sess structure BB */
	pSMB->t2.MaxDataCount = cpu_to_le16(CIFSMaxBufSize);
	pSMB->t2.MaxSetupCount = 0;
	pSMB->t2.Reserved = 0;
	pSMB->t2.Flags = 0;
	pSMB->t2.Timeout = 0;
	pSMB->t2.Reserved2 = 0;
	pSMB->t2.ParameterOffset = cpu_to_le16(offsetof(struct smb_t2_qfi_req,
					       Fid) - 4);
	pSMB->t2.DataCount = 0;
	pSMB->t2.DataOffset = 0;
	pSMB->t2.SetupCount = 1;
	pSMB->t2.Reserved3 = 0;
	pSMB->t2.SubCommand = cpu_to_le16(TRANS2_QUERY_FILE_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->t2.TotalParameterCount = cpu_to_le16(params);
	pSMB->t2.ParameterCount = pSMB->t2.TotalParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FILE_ALL_INFO);
	pSMB->Pad = 0;
	pSMB->Fid = netfid;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->t2.ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QFileInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc) /* BB add auto retry on EOPNOTSUPP? */
			rc = -EIO;
		else if (get_bcc(&pSMBr->hdr) < 40)
			rc = -EIO;	/* bad smb */
		else if (pFindData) {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			memcpy((char *) pFindData,
			       (char *) &pSMBr->hdr.Protocol +
			       data_offset, sizeof(FILE_ALL_INFO));
		} else
		    rc = -ENOMEM;
	}
	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto QFileInfoRetry;

	return rc;
}

int
CIFSSMBQPathInfo(const unsigned int xid, struct cifs_tcon *tcon,
		 const char *search_name, FILE_ALL_INFO *data,
		 int legacy /* old style infolevel */,
		 const struct nls_table *nls_codepage, int remap)
{
	/* level 263 SMB_QUERY_FILE_ALL_INFO */
	TRANSACTION2_QPI_REQ *pSMB = NULL;
	TRANSACTION2_QPI_RSP *pSMBr = NULL;
	int rc = 0;
	int bytes_returned;
	int name_len;
	__u16 params, byte_count;

	/* cifs_dbg(FYI, "In QPathInfo path %s\n", search_name); */
QPathInfoRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, search_name,
				       PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, search_name);
	}

	params = 2 /* level */ + 4 /* reserved */ + name_len /* includes NUL */;
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(4000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
	struct smb_com_transaction2_qpi_req, InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_PATH_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	if (legacy)
		pSMB->InformationLevel = cpu_to_le16(SMB_INFO_STANDARD);
	else
		pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FILE_ALL_INFO);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QPathInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc) /* BB add auto retry on EOPNOTSUPP? */
			rc = -EIO;
		else if (!legacy && get_bcc(&pSMBr->hdr) < 40)
			rc = -EIO;	/* bad smb */
		else if (legacy && get_bcc(&pSMBr->hdr) < 24)
			rc = -EIO;  /* 24 or 26 expected but we do not read
					last field */
		else if (data) {
			int size;
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);

			/*
			 * On legacy responses we do not read the last field,
			 * EAsize, fortunately since it varies by subdialect and
			 * also note it differs on Set vs Get, ie two bytes or 4
			 * bytes depending but we don't care here.
			 */
			if (legacy)
				size = sizeof(FILE_INFO_STANDARD);
			else
				size = sizeof(FILE_ALL_INFO);
			memcpy((char *) data, (char *) &pSMBr->hdr.Protocol +
			       data_offset, size);
		} else
		    rc = -ENOMEM;
	}
	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto QPathInfoRetry;

	return rc;
}

int
CIFSSMBUnixQFileInfo(const unsigned int xid, struct cifs_tcon *tcon,
		 u16 netfid, FILE_UNIX_BASIC_INFO *pFindData)
{
	struct smb_t2_qfi_req *pSMB = NULL;
	struct smb_t2_qfi_rsp *pSMBr = NULL;
	int rc = 0;
	int bytes_returned;
	__u16 params, byte_count;

UnixQFileInfoRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	params = 2 /* level */ + 2 /* fid */;
	pSMB->t2.TotalDataCount = 0;
	pSMB->t2.MaxParameterCount = cpu_to_le16(4);
	/* BB find exact max data count below from sess structure BB */
	pSMB->t2.MaxDataCount = cpu_to_le16(CIFSMaxBufSize);
	pSMB->t2.MaxSetupCount = 0;
	pSMB->t2.Reserved = 0;
	pSMB->t2.Flags = 0;
	pSMB->t2.Timeout = 0;
	pSMB->t2.Reserved2 = 0;
	pSMB->t2.ParameterOffset = cpu_to_le16(offsetof(struct smb_t2_qfi_req,
					       Fid) - 4);
	pSMB->t2.DataCount = 0;
	pSMB->t2.DataOffset = 0;
	pSMB->t2.SetupCount = 1;
	pSMB->t2.Reserved3 = 0;
	pSMB->t2.SubCommand = cpu_to_le16(TRANS2_QUERY_FILE_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->t2.TotalParameterCount = cpu_to_le16(params);
	pSMB->t2.ParameterCount = pSMB->t2.TotalParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FILE_UNIX_BASIC);
	pSMB->Pad = 0;
	pSMB->Fid = netfid;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->t2.ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in UnixQFileInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) < sizeof(FILE_UNIX_BASIC_INFO)) {
			cifs_dbg(VFS, "Malformed FILE_UNIX_BASIC_INFO response. Unix Extensions can be disabled on mount by specifying the nosfu mount option.\n");
			rc = -EIO;	/* bad smb */
		} else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			memcpy((char *) pFindData,
			       (char *) &pSMBr->hdr.Protocol +
			       data_offset,
			       sizeof(FILE_UNIX_BASIC_INFO));
		}
	}

	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto UnixQFileInfoRetry;

	return rc;
}

int
CIFSSMBUnixQPathInfo(const unsigned int xid, struct cifs_tcon *tcon,
		     const unsigned char *searchName,
		     FILE_UNIX_BASIC_INFO *pFindData,
		     const struct nls_table *nls_codepage, int remap)
{
/* SMB_QUERY_FILE_UNIX_BASIC */
	TRANSACTION2_QPI_REQ *pSMB = NULL;
	TRANSACTION2_QPI_RSP *pSMBr = NULL;
	int rc = 0;
	int bytes_returned = 0;
	int name_len;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In QPathInfo (Unix) the path %s\n", searchName);
UnixQPathInfoRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, searchName,
				       PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, searchName);
	}

	params = 2 /* level */ + 4 /* reserved */ + name_len /* includes NUL */;
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(4000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
	struct smb_com_transaction2_qpi_req, InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_PATH_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FILE_UNIX_BASIC);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in UnixQPathInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) < sizeof(FILE_UNIX_BASIC_INFO)) {
			cifs_dbg(VFS, "Malformed FILE_UNIX_BASIC_INFO response. Unix Extensions can be disabled on mount by specifying the nosfu mount option.\n");
			rc = -EIO;	/* bad smb */
		} else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			memcpy((char *) pFindData,
			       (char *) &pSMBr->hdr.Protocol +
			       data_offset,
			       sizeof(FILE_UNIX_BASIC_INFO));
		}
	}
	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto UnixQPathInfoRetry;

	return rc;
}

/* xid, tcon, searchName and codepage are input parms, rest are returned */
int
CIFSFindFirst(const unsigned int xid, struct cifs_tcon *tcon,
	      const char *searchName, struct cifs_sb_info *cifs_sb,
	      __u16 *pnetfid, __u16 search_flags,
	      struct cifs_search_info *psrch_inf, bool msearch)
{
/* level 257 SMB_ */
	TRANSACTION2_FFIRST_REQ *pSMB = NULL;
	TRANSACTION2_FFIRST_RSP *pSMBr = NULL;
	T2_FFIRST_RSP_PARMS *parms;
	struct nls_table *nls_codepage;
	unsigned int lnoff;
	__u16 params, byte_count;
	int bytes_returned = 0;
	int name_len, remap;
	int rc = 0;

	cifs_dbg(FYI, "In FindFirst for %s\n", searchName);

findFirstRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	nls_codepage = cifs_sb->local_nls;
	remap = cifs_remap(cifs_sb);

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, searchName,
				       PATH_MAX, nls_codepage, remap);
		/* We can not add the asterisk earlier in case
		it got remapped to 0xF03A as if it were part of the
		directory name instead of a wildcard */
		name_len *= 2;
		if (msearch) {
			pSMB->FileName[name_len] = CIFS_DIR_SEP(cifs_sb);
			pSMB->FileName[name_len+1] = 0;
			pSMB->FileName[name_len+2] = '*';
			pSMB->FileName[name_len+3] = 0;
			name_len += 4; /* now the trailing null */
			/* null terminate just in case */
			pSMB->FileName[name_len] = 0;
			pSMB->FileName[name_len+1] = 0;
			name_len += 2;
		} else if (!searchName[0]) {
			pSMB->FileName[0] = CIFS_DIR_SEP(cifs_sb);
			pSMB->FileName[1] = 0;
			pSMB->FileName[2] = 0;
			pSMB->FileName[3] = 0;
			name_len = 4;
		}
	} else {
		name_len = copy_path_name(pSMB->FileName, searchName);
		if (msearch) {
			if (WARN_ON_ONCE(name_len > PATH_MAX-2))
				name_len = PATH_MAX-2;
			/* overwrite nul byte */
			pSMB->FileName[name_len-1] = CIFS_DIR_SEP(cifs_sb);
			pSMB->FileName[name_len] = '*';
			pSMB->FileName[name_len+1] = 0;
			name_len += 2;
		} else if (!searchName[0]) {
			pSMB->FileName[0] = CIFS_DIR_SEP(cifs_sb);
			pSMB->FileName[1] = 0;
			name_len = 2;
		}
	}

	params = 12 + name_len /* includes null */ ;
	pSMB->TotalDataCount = 0;	/* no EAs */
	pSMB->MaxParameterCount = cpu_to_le16(10);
	pSMB->MaxDataCount = cpu_to_le16(CIFSMaxBufSize & 0xFFFFFF00);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(
	      offsetof(struct smb_com_transaction2_ffirst_req, SearchAttributes)
		- 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;	/* one byte, no need to make endian neutral */
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_FIND_FIRST);
	pSMB->SearchAttributes =
	    cpu_to_le16(ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM |
			ATTR_DIRECTORY);
	pSMB->SearchCount = cpu_to_le16(msearch ? CIFSMaxBufSize/sizeof(FILE_UNIX_INFO) : 1);
	pSMB->SearchFlags = cpu_to_le16(search_flags);
	pSMB->InformationLevel = cpu_to_le16(psrch_inf->info_level);

	/* BB what should we set StorageType to? Does it matter? BB */
	pSMB->SearchStorageType = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_ffirst);

	if (rc) {
		/*
		 * BB: add logic to retry regular search if Unix search rejected
		 * unexpectedly by server.
		 */
		/* BB: add code to handle unsupported level rc */
		cifs_dbg(FYI, "Error in FindFirst = %d\n", rc);
		cifs_buf_release(pSMB);
		/*
		 * BB: eventually could optimize out free and realloc of buf for
		 * this case.
		 */
		if (rc == -EAGAIN)
			goto findFirstRetry;
		return rc;
	}
	/* decode response */
	rc = validate_t2((struct smb_t2_rsp *)pSMBr);
	if (rc) {
		cifs_buf_release(pSMB);
		return rc;
	}

	psrch_inf->unicode = !!(pSMBr->hdr.Flags2 & SMBFLG2_UNICODE);
	psrch_inf->ntwrk_buf_start = (char *)pSMBr;
	psrch_inf->smallBuf = false;
	psrch_inf->srch_entries_start = (char *)&pSMBr->hdr.Protocol +
		le16_to_cpu(pSMBr->t2.DataOffset);

	parms = (T2_FFIRST_RSP_PARMS *)((char *)&pSMBr->hdr.Protocol +
					le16_to_cpu(pSMBr->t2.ParameterOffset));
	psrch_inf->endOfSearch = !!parms->EndofSearch;

	psrch_inf->entries_in_buffer = le16_to_cpu(parms->SearchCount);
	psrch_inf->index_of_last_entry = 2 /* skip . and .. */ +
		psrch_inf->entries_in_buffer;
	lnoff = le16_to_cpu(parms->LastNameOffset);
	if (CIFSMaxBufSize < lnoff) {
		cifs_dbg(VFS, "ignoring corrupt resume name\n");
		psrch_inf->last_entry = NULL;
	} else {
		psrch_inf->last_entry = psrch_inf->srch_entries_start + lnoff;
		if (pnetfid)
			*pnetfid = parms->SearchHandle;
	}
	return 0;
}

int CIFSFindNext(const unsigned int xid, struct cifs_tcon *tcon,
		 __u16 searchHandle, __u16 search_flags,
		 struct cifs_search_info *psrch_inf)
{
	TRANSACTION2_FNEXT_REQ *pSMB = NULL;
	TRANSACTION2_FNEXT_RSP *pSMBr = NULL;
	T2_FNEXT_RSP_PARMS *parms;
	unsigned int name_len;
	unsigned int lnoff;
	__u16 params, byte_count;
	char *response_data;
	int bytes_returned;
	int rc = 0;

	cifs_dbg(FYI, "In FindNext\n");

	if (psrch_inf->endOfSearch)
		return -ENOENT;

	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		(void **) &pSMBr);
	if (rc)
		return rc;

	params = 14; /* includes 2 bytes of null string, converted to LE below*/
	byte_count = 0;
	pSMB->TotalDataCount = 0;       /* no EAs */
	pSMB->MaxParameterCount = cpu_to_le16(8);
	pSMB->MaxDataCount = cpu_to_le16(CIFSMaxBufSize & 0xFFFFFF00);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	pSMB->ParameterOffset =  cpu_to_le16(
	      offsetof(struct smb_com_transaction2_fnext_req,SearchHandle) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_FIND_NEXT);
	pSMB->SearchHandle = searchHandle;      /* always kept as le */
	pSMB->SearchCount =
		cpu_to_le16(CIFSMaxBufSize / sizeof(FILE_UNIX_INFO));
	pSMB->InformationLevel = cpu_to_le16(psrch_inf->info_level);
	pSMB->ResumeKey = psrch_inf->resume_key;
	pSMB->SearchFlags = cpu_to_le16(search_flags);

	name_len = psrch_inf->resume_name_len;
	params += name_len;
	if (name_len < PATH_MAX) {
		memcpy(pSMB->ResumeFileName, psrch_inf->presume_name, name_len);
		byte_count += name_len;
		/* 14 byte parm len above enough for 2 byte null terminator */
		pSMB->ResumeFileName[name_len] = 0;
		pSMB->ResumeFileName[name_len+1] = 0;
	} else {
		cifs_buf_release(pSMB);
		return -EINVAL;
	}
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			(struct smb_hdr *) pSMBr, &bytes_returned, 0);
	cifs_stats_inc(&tcon->stats.cifs_stats.num_fnext);

	if (rc) {
		cifs_buf_release(pSMB);
		if (rc == -EBADF) {
			psrch_inf->endOfSearch = true;
			rc = 0; /* search probably was closed at end of search*/
		} else {
			cifs_dbg(FYI, "FindNext returned = %d\n", rc);
		}
		return rc;
	}

	/* decode response */
	rc = validate_t2((struct smb_t2_rsp *)pSMBr);
	if (rc) {
		cifs_buf_release(pSMB);
		return rc;
	}
	/* BB fixme add lock for file (srch_info) struct here */
	psrch_inf->unicode = !!(pSMBr->hdr.Flags2 & SMBFLG2_UNICODE);
	response_data = (char *)&pSMBr->hdr.Protocol +
		le16_to_cpu(pSMBr->t2.ParameterOffset);
	parms = (T2_FNEXT_RSP_PARMS *)response_data;
	response_data = (char *)&pSMBr->hdr.Protocol +
		le16_to_cpu(pSMBr->t2.DataOffset);

	if (psrch_inf->smallBuf)
		cifs_small_buf_release(psrch_inf->ntwrk_buf_start);
	else
		cifs_buf_release(psrch_inf->ntwrk_buf_start);

	psrch_inf->srch_entries_start = response_data;
	psrch_inf->ntwrk_buf_start = (char *)pSMB;
	psrch_inf->smallBuf = false;
	psrch_inf->endOfSearch = !!parms->EndofSearch;
	psrch_inf->entries_in_buffer = le16_to_cpu(parms->SearchCount);
	psrch_inf->index_of_last_entry += psrch_inf->entries_in_buffer;
	lnoff = le16_to_cpu(parms->LastNameOffset);
	if (CIFSMaxBufSize < lnoff) {
		cifs_dbg(VFS, "ignoring corrupt resume name\n");
		psrch_inf->last_entry = NULL;
	} else {
		psrch_inf->last_entry =
			psrch_inf->srch_entries_start + lnoff;
	}
	/* BB fixme add unlock here */

	/*
	 * BB: On error, should we leave previous search buf
	 * (and count and last entry fields) intact or free the previous one?
	 *
	 * Note: On -EAGAIN error only caller can retry on handle based calls
	 * since file handle passed in no longer valid.
	 */
	return 0;
}

int
CIFSFindClose(const unsigned int xid, struct cifs_tcon *tcon,
	      const __u16 searchHandle)
{
	int rc = 0;
	FINDCLOSE_REQ *pSMB = NULL;

	cifs_dbg(FYI, "In CIFSSMBFindClose\n");
	rc = small_smb_init(SMB_COM_FIND_CLOSE2, 1, tcon, (void **)&pSMB);

	/* no sense returning error if session restarted
		as file handle has been closed */
	if (rc == -EAGAIN)
		return 0;
	if (rc)
		return rc;

	pSMB->FileID = searchHandle;
	pSMB->ByteCount = 0;
	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) pSMB, 0);
	cifs_small_buf_release(pSMB);
	if (rc)
		cifs_dbg(VFS, "Send error in FindClose = %d\n", rc);

	cifs_stats_inc(&tcon->stats.cifs_stats.num_fclose);

	/* Since session is dead, search handle closed on server already */
	if (rc == -EAGAIN)
		rc = 0;

	return rc;
}

int
CIFSGetSrvInodeNumber(const unsigned int xid, struct cifs_tcon *tcon,
		      const char *search_name, __u64 *inode_number,
		      const struct nls_table *nls_codepage, int remap)
{
	int rc = 0;
	TRANSACTION2_QPI_REQ *pSMB = NULL;
	TRANSACTION2_QPI_RSP *pSMBr = NULL;
	int name_len, bytes_returned;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In GetSrvInodeNum for %s\n", search_name);
	if (tcon == NULL)
		return -ENODEV;

GetInodeNumberRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
			cifsConvertToUTF16((__le16 *) pSMB->FileName,
					   search_name, PATH_MAX, nls_codepage,
					   remap);
		name_len++;     /* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, search_name);
	}

	params = 2 /* level */  + 4 /* rsrvd */  + name_len /* incl null */ ;
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max data count below from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(4000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
		struct smb_com_transaction2_qpi_req, InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_PATH_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FILE_INTERNAL_INFO);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
		(struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "error %d in QueryInternalInfo\n", rc);
	} else {
		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);
		/* BB also check enough total bytes returned */
		if (rc || get_bcc(&pSMBr->hdr) < 2)
			/* If rc should we check for EOPNOSUPP and
			disable the srvino flag? or in caller? */
			rc = -EIO;      /* bad smb */
		else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			__u16 count = le16_to_cpu(pSMBr->t2.DataCount);
			struct file_internal_info *pfinfo;
			/* BB Do we need a cast or hash here ? */
			if (count < 8) {
				cifs_dbg(FYI, "Invalid size ret in QryIntrnlInf\n");
				rc = -EIO;
				goto GetInodeNumOut;
			}
			pfinfo = (struct file_internal_info *)
				(data_offset + (char *) &pSMBr->hdr.Protocol);
			*inode_number = le64_to_cpu(pfinfo->UniqueId);
		}
	}
GetInodeNumOut:
	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto GetInodeNumberRetry;
	return rc;
}

int
CIFSGetDFSRefer(const unsigned int xid, struct cifs_ses *ses,
		const char *search_name, struct dfs_info3_param **target_nodes,
		unsigned int *num_of_nodes,
		const struct nls_table *nls_codepage, int remap)
{
/* TRANS2_GET_DFS_REFERRAL */
	TRANSACTION2_GET_DFS_REFER_REQ *pSMB = NULL;
	TRANSACTION2_GET_DFS_REFER_RSP *pSMBr = NULL;
	int rc = 0;
	int bytes_returned;
	int name_len;
	__u16 params, byte_count;
	*num_of_nodes = 0;
	*target_nodes = NULL;

	cifs_dbg(FYI, "In GetDFSRefer the path %s\n", search_name);
	if (ses == NULL || ses->tcon_ipc == NULL)
		return -ENODEV;

getDFSRetry:
	/*
	 * Use smb_init_no_reconnect() instead of smb_init() as
	 * CIFSGetDFSRefer() may be called from cifs_reconnect_tcon() and thus
	 * causing an infinite recursion.
	 */
	rc = smb_init(SMB_COM_TRANSACTION2, 15, ses->tcon_ipc,
		      (void **)&pSMB, (void **)&pSMBr);
	if (rc)
		return rc;

	/* server pointer checked in called function,
	but should never be null here anyway */
	pSMB->hdr.Mid = get_next_mid(ses->server);
	pSMB->hdr.Tid = ses->tcon_ipc->tid;
	pSMB->hdr.Uid = ses->Suid;
	if (ses->capabilities & CAP_STATUS32)
		pSMB->hdr.Flags2 |= SMBFLG2_ERR_STATUS;
	if (ses->capabilities & CAP_DFS)
		pSMB->hdr.Flags2 |= SMBFLG2_DFS;

	if (ses->capabilities & CAP_UNICODE) {
		pSMB->hdr.Flags2 |= SMBFLG2_UNICODE;
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->RequestFileName,
				       search_name, PATH_MAX, nls_codepage,
				       remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {	/* BB improve the check for buffer overruns BB */
		name_len = copy_path_name(pSMB->RequestFileName, search_name);
	}

	if (ses->server->sign)
		pSMB->hdr.Flags2 |= SMBFLG2_SECURITY_SIGNATURE;

	pSMB->hdr.Uid = ses->Suid;

	params = 2 /* level */  + name_len /*includes null */ ;
	pSMB->TotalDataCount = 0;
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->MaxParameterCount = 0;
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(4000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
	  struct smb_com_transaction2_get_dfs_refer_req, MaxReferralLevel) - 4);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_GET_DFS_REFERRAL);
	byte_count = params + 3 /* pad */ ;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->MaxReferralLevel = cpu_to_le16(3);
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in GetDFSRefer = %d\n", rc);
		goto GetDFSRefExit;
	}
	rc = validate_t2((struct smb_t2_rsp *)pSMBr);

	/* BB Also check if enough total bytes returned? */
	if (rc || get_bcc(&pSMBr->hdr) < 17) {
		rc = -EIO;      /* bad smb */
		goto GetDFSRefExit;
	}

	cifs_dbg(FYI, "Decoding GetDFSRefer response BCC: %d  Offset %d\n",
		 get_bcc(&pSMBr->hdr), le16_to_cpu(pSMBr->t2.DataOffset));

	/* parse returned result into more usable form */
	rc = parse_dfs_referrals(&pSMBr->dfs_data,
				 le16_to_cpu(pSMBr->t2.DataCount),
				 num_of_nodes, target_nodes, nls_codepage,
				 remap, search_name,
				 (pSMBr->hdr.Flags2 & SMBFLG2_UNICODE) != 0);

GetDFSRefExit:
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto getDFSRetry;

	return rc;
}

/* Query File System Info such as free space to old servers such as Win 9x */
int
SMBOldQFSInfo(const unsigned int xid, struct cifs_tcon *tcon,
	      struct kstatfs *FSData)
{
/* level 0x01 SMB_QUERY_FILE_SYSTEM_INFO */
	TRANSACTION2_QFSI_REQ *pSMB = NULL;
	TRANSACTION2_QFSI_RSP *pSMBr = NULL;
	FILE_SYSTEM_ALLOC_INFO *response_data;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, byte_count;

	cifs_dbg(FYI, "OldQFSInfo\n");
oldQFSInfoRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		(void **) &pSMBr);
	if (rc)
		return rc;

	params = 2;     /* level */
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
	struct smb_com_transaction2_qfsi_req, InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_FS_INFORMATION);
	pSMB->InformationLevel = cpu_to_le16(SMB_INFO_ALLOCATION);
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
		(struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QFSInfo = %d\n", rc);
	} else {                /* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) < 18)
			rc = -EIO;      /* bad smb */
		else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			cifs_dbg(FYI, "qfsinf resp BCC: %d  Offset %d\n",
				 get_bcc(&pSMBr->hdr), data_offset);

			response_data = (FILE_SYSTEM_ALLOC_INFO *)
				(((char *) &pSMBr->hdr.Protocol) + data_offset);
			FSData->f_bsize =
				le16_to_cpu(response_data->BytesPerSector) *
				le32_to_cpu(response_data->
					SectorsPerAllocationUnit);
			/*
			 * much prefer larger but if server doesn't report
			 * a valid size than 4K is a reasonable minimum
			 */
			if (FSData->f_bsize < 512)
				FSData->f_bsize = 4096;

			FSData->f_blocks =
			       le32_to_cpu(response_data->TotalAllocationUnits);
			FSData->f_bfree = FSData->f_bavail =
				le32_to_cpu(response_data->FreeAllocationUnits);
			cifs_dbg(FYI, "Blocks: %lld  Free: %lld Block size %ld\n",
				 (unsigned long long)FSData->f_blocks,
				 (unsigned long long)FSData->f_bfree,
				 FSData->f_bsize);
		}
	}
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto oldQFSInfoRetry;

	return rc;
}

int
CIFSSMBQFSInfo(const unsigned int xid, struct cifs_tcon *tcon,
	       struct kstatfs *FSData)
{
/* level 0x103 SMB_QUERY_FILE_SYSTEM_INFO */
	TRANSACTION2_QFSI_REQ *pSMB = NULL;
	TRANSACTION2_QFSI_RSP *pSMBr = NULL;
	FILE_SYSTEM_INFO *response_data;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In QFSInfo\n");
QFSInfoRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	params = 2;	/* level */
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
		struct smb_com_transaction2_qfsi_req, InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_FS_INFORMATION);
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FS_SIZE_INFO);
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QFSInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) < 24)
			rc = -EIO;	/* bad smb */
		else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);

			response_data =
			    (FILE_SYSTEM_INFO
			     *) (((char *) &pSMBr->hdr.Protocol) +
				 data_offset);
			FSData->f_bsize =
			    le32_to_cpu(response_data->BytesPerSector) *
			    le32_to_cpu(response_data->
					SectorsPerAllocationUnit);
			/*
			 * much prefer larger but if server doesn't report
			 * a valid size than 4K is a reasonable minimum
			 */
			if (FSData->f_bsize < 512)
				FSData->f_bsize = 4096;

			FSData->f_blocks =
			    le64_to_cpu(response_data->TotalAllocationUnits);
			FSData->f_bfree = FSData->f_bavail =
			    le64_to_cpu(response_data->FreeAllocationUnits);
			cifs_dbg(FYI, "Blocks: %lld  Free: %lld Block size %ld\n",
				 (unsigned long long)FSData->f_blocks,
				 (unsigned long long)FSData->f_bfree,
				 FSData->f_bsize);
		}
	}
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto QFSInfoRetry;

	return rc;
}

int
CIFSSMBQFSAttributeInfo(const unsigned int xid, struct cifs_tcon *tcon)
{
/* level 0x105  SMB_QUERY_FILE_SYSTEM_INFO */
	TRANSACTION2_QFSI_REQ *pSMB = NULL;
	TRANSACTION2_QFSI_RSP *pSMBr = NULL;
	FILE_SYSTEM_ATTRIBUTE_INFO *response_data;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In QFSAttributeInfo\n");
QFSAttributeRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	params = 2;	/* level */
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
		struct smb_com_transaction2_qfsi_req, InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_FS_INFORMATION);
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FS_ATTRIBUTE_INFO);
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(VFS, "Send error in QFSAttributeInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) < 13) {
			/* BB also check if enough bytes returned */
			rc = -EIO;	/* bad smb */
		} else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			response_data =
			    (FILE_SYSTEM_ATTRIBUTE_INFO
			     *) (((char *) &pSMBr->hdr.Protocol) +
				 data_offset);
			memcpy(&tcon->fsAttrInfo, response_data,
			       sizeof(FILE_SYSTEM_ATTRIBUTE_INFO));
		}
	}
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto QFSAttributeRetry;

	return rc;
}

int
CIFSSMBQFSDeviceInfo(const unsigned int xid, struct cifs_tcon *tcon)
{
/* level 0x104 SMB_QUERY_FILE_SYSTEM_INFO */
	TRANSACTION2_QFSI_REQ *pSMB = NULL;
	TRANSACTION2_QFSI_RSP *pSMBr = NULL;
	FILE_SYSTEM_DEVICE_INFO *response_data;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In QFSDeviceInfo\n");
QFSDeviceRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	params = 2;	/* level */
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
		struct smb_com_transaction2_qfsi_req, InformationLevel) - 4);

	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_FS_INFORMATION);
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_FS_DEVICE_INFO);
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QFSDeviceInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) <
			  sizeof(FILE_SYSTEM_DEVICE_INFO))
			rc = -EIO;	/* bad smb */
		else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			response_data =
			    (FILE_SYSTEM_DEVICE_INFO *)
				(((char *) &pSMBr->hdr.Protocol) +
				 data_offset);
			memcpy(&tcon->fsDevInfo, response_data,
			       sizeof(FILE_SYSTEM_DEVICE_INFO));
		}
	}
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto QFSDeviceRetry;

	return rc;
}

int
CIFSSMBQFSUnixInfo(const unsigned int xid, struct cifs_tcon *tcon)
{
/* level 0x200  SMB_QUERY_CIFS_UNIX_INFO */
	TRANSACTION2_QFSI_REQ *pSMB = NULL;
	TRANSACTION2_QFSI_RSP *pSMBr = NULL;
	FILE_SYSTEM_UNIX_INFO *response_data;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In QFSUnixInfo\n");
QFSUnixRetry:
	rc = smb_init_no_reconnect(SMB_COM_TRANSACTION2, 15, tcon,
				   (void **) &pSMB, (void **) &pSMBr);
	if (rc)
		return rc;

	params = 2;	/* level */
	pSMB->TotalDataCount = 0;
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(100);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	byte_count = params + 1 /* pad */ ;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(struct
			smb_com_transaction2_qfsi_req, InformationLevel) - 4);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_FS_INFORMATION);
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_CIFS_UNIX_INFO);
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(VFS, "Send error in QFSUnixInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) < 13) {
			rc = -EIO;	/* bad smb */
		} else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			response_data =
			    (FILE_SYSTEM_UNIX_INFO
			     *) (((char *) &pSMBr->hdr.Protocol) +
				 data_offset);
			memcpy(&tcon->fsUnixInfo, response_data,
			       sizeof(FILE_SYSTEM_UNIX_INFO));
		}
	}
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto QFSUnixRetry;


	return rc;
}

int
CIFSSMBSetFSUnixInfo(const unsigned int xid, struct cifs_tcon *tcon, __u64 cap)
{
/* level 0x200  SMB_SET_CIFS_UNIX_INFO */
	TRANSACTION2_SETFSI_REQ *pSMB = NULL;
	TRANSACTION2_SETFSI_RSP *pSMBr = NULL;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, param_offset, offset, byte_count;

	cifs_dbg(FYI, "In SETFSUnixInfo\n");
SETFSUnixRetry:
	/* BB switch to small buf init to save memory */
	rc = smb_init_no_reconnect(SMB_COM_TRANSACTION2, 15, tcon,
					(void **) &pSMB, (void **) &pSMBr);
	if (rc)
		return rc;

	params = 4;	/* 2 bytes zero followed by info level. */
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_setfsi_req, FileNum)
				- 4;
	offset = param_offset + params;

	pSMB->MaxParameterCount = cpu_to_le16(4);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(100);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_FS_INFORMATION);
	byte_count = 1 /* pad */ + params + 12;

	pSMB->DataCount = cpu_to_le16(12);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);

	/* Params. */
	pSMB->FileNum = 0;
	pSMB->InformationLevel = cpu_to_le16(SMB_SET_CIFS_UNIX_INFO);

	/* Data. */
	pSMB->ClientUnixMajor = cpu_to_le16(CIFS_UNIX_MAJOR_VERSION);
	pSMB->ClientUnixMinor = cpu_to_le16(CIFS_UNIX_MINOR_VERSION);
	pSMB->ClientUnixCap = cpu_to_le64(cap);

	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(VFS, "Send error in SETFSUnixInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);
		if (rc)
			rc = -EIO;	/* bad smb */
	}
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto SETFSUnixRetry;

	return rc;
}



int
CIFSSMBQFSPosixInfo(const unsigned int xid, struct cifs_tcon *tcon,
		   struct kstatfs *FSData)
{
/* level 0x201  SMB_QUERY_CIFS_POSIX_INFO */
	TRANSACTION2_QFSI_REQ *pSMB = NULL;
	TRANSACTION2_QFSI_RSP *pSMBr = NULL;
	FILE_SYSTEM_POSIX_INFO *response_data;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, byte_count;

	cifs_dbg(FYI, "In QFSPosixInfo\n");
QFSPosixRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	params = 2;	/* level */
	pSMB->TotalDataCount = 0;
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(100);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	byte_count = params + 1 /* pad */ ;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(struct
			smb_com_transaction2_qfsi_req, InformationLevel) - 4);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_FS_INFORMATION);
	pSMB->InformationLevel = cpu_to_le16(SMB_QUERY_POSIX_FS_INFO);
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QFSUnixInfo = %d\n", rc);
	} else {		/* decode response */
		rc = validate_t2((struct smb_t2_rsp *)pSMBr);

		if (rc || get_bcc(&pSMBr->hdr) < 13) {
			rc = -EIO;	/* bad smb */
		} else {
			__u16 data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
			response_data =
			    (FILE_SYSTEM_POSIX_INFO
			     *) (((char *) &pSMBr->hdr.Protocol) +
				 data_offset);
			FSData->f_bsize =
					le32_to_cpu(response_data->BlockSize);
			/*
			 * much prefer larger but if server doesn't report
			 * a valid size than 4K is a reasonable minimum
			 */
			if (FSData->f_bsize < 512)
				FSData->f_bsize = 4096;

			FSData->f_blocks =
					le64_to_cpu(response_data->TotalBlocks);
			FSData->f_bfree =
			    le64_to_cpu(response_data->BlocksAvail);
			if (response_data->UserBlocksAvail == cpu_to_le64(-1)) {
				FSData->f_bavail = FSData->f_bfree;
			} else {
				FSData->f_bavail =
				    le64_to_cpu(response_data->UserBlocksAvail);
			}
			if (response_data->TotalFileNodes != cpu_to_le64(-1))
				FSData->f_files =
				     le64_to_cpu(response_data->TotalFileNodes);
			if (response_data->FreeFileNodes != cpu_to_le64(-1))
				FSData->f_ffree =
				      le64_to_cpu(response_data->FreeFileNodes);
		}
	}
	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto QFSPosixRetry;

	return rc;
}


/*
 * We can not use write of zero bytes trick to set file size due to need for
 * large file support. Also note that this SetPathInfo is preferred to
 * SetFileInfo based method in next routine which is only needed to work around
 * a sharing violation bugin Samba which this routine can run into.
 */
int
CIFSSMBSetEOF(const unsigned int xid, struct cifs_tcon *tcon,
	      const char *file_name, __u64 size, struct cifs_sb_info *cifs_sb,
	      bool set_allocation, struct dentry *dentry)
{
	struct smb_com_transaction2_spi_req *pSMB = NULL;
	struct smb_com_transaction2_spi_rsp *pSMBr = NULL;
	struct file_end_of_file_info *parm_data;
	int name_len;
	int rc = 0;
	int bytes_returned = 0;
	int remap = cifs_remap(cifs_sb);

	__u16 params, byte_count, data_count, param_offset, offset;

	cifs_dbg(FYI, "In SetEOF\n");
SetEOFRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, file_name,
				       PATH_MAX, cifs_sb->local_nls, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, file_name);
	}
	params = 6 + name_len;
	data_count = sizeof(struct file_end_of_file_info);
	pSMB->MaxParameterCount = cpu_to_le16(2);
	pSMB->MaxDataCount = cpu_to_le16(4100);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;
	if (set_allocation) {
		if (tcon->ses->capabilities & CAP_INFOLEVEL_PASSTHRU)
			pSMB->InformationLevel =
				cpu_to_le16(SMB_SET_FILE_ALLOCATION_INFO2);
		else
			pSMB->InformationLevel =
				cpu_to_le16(SMB_SET_FILE_ALLOCATION_INFO);
	} else /* Set File Size */  {
	    if (tcon->ses->capabilities & CAP_INFOLEVEL_PASSTHRU)
		    pSMB->InformationLevel =
				cpu_to_le16(SMB_SET_FILE_END_OF_FILE_INFO2);
	    else
		    pSMB->InformationLevel =
				cpu_to_le16(SMB_SET_FILE_END_OF_FILE_INFO);
	}

	parm_data =
	    (struct file_end_of_file_info *) (((char *) &pSMB->hdr.Protocol) +
				       offset);
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	byte_count = 3 /* pad */  + params + data_count;
	pSMB->DataCount = cpu_to_le16(data_count);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	parm_data->FileSize = cpu_to_le64(size);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc)
		cifs_dbg(FYI, "SetPathInfo (file size) returned %d\n", rc);

	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto SetEOFRetry;

	return rc;
}

int
CIFSSMBSetFileSize(const unsigned int xid, struct cifs_tcon *tcon,
		   struct cifsFileInfo *cfile, __u64 size, bool set_allocation)
{
	struct smb_com_transaction2_sfi_req *pSMB  = NULL;
	struct file_end_of_file_info *parm_data;
	int rc = 0;
	__u16 params, param_offset, offset, byte_count, count;

	cifs_dbg(FYI, "SetFileSize (via SetFileInfo) %lld\n",
		 (long long)size);
	rc = small_smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB);

	if (rc)
		return rc;

	pSMB->hdr.Pid = cpu_to_le16((__u16)cfile->pid);
	pSMB->hdr.PidHigh = cpu_to_le16((__u16)(cfile->pid >> 16));

	params = 6;
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_sfi_req, Fid) - 4;
	offset = param_offset + params;

	count = sizeof(struct file_end_of_file_info);
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_FILE_INFORMATION);
	byte_count = 3 /* pad */  + params + count;
	pSMB->DataCount = cpu_to_le16(count);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	/* SMB offsets are from the beginning of SMB which is 4 bytes in, after RFC1001 field */
	parm_data =
		(struct file_end_of_file_info *)(((char *)pSMB) + offset + 4);
	pSMB->DataOffset = cpu_to_le16(offset);
	parm_data->FileSize = cpu_to_le64(size);
	pSMB->Fid = cfile->fid.netfid;
	if (set_allocation) {
		if (tcon->ses->capabilities & CAP_INFOLEVEL_PASSTHRU)
			pSMB->InformationLevel =
				cpu_to_le16(SMB_SET_FILE_ALLOCATION_INFO2);
		else
			pSMB->InformationLevel =
				cpu_to_le16(SMB_SET_FILE_ALLOCATION_INFO);
	} else /* Set File Size */  {
	    if (tcon->ses->capabilities & CAP_INFOLEVEL_PASSTHRU)
		    pSMB->InformationLevel =
				cpu_to_le16(SMB_SET_FILE_END_OF_FILE_INFO2);
	    else
		    pSMB->InformationLevel =
				cpu_to_le16(SMB_SET_FILE_END_OF_FILE_INFO);
	}
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) pSMB, 0);
	cifs_small_buf_release(pSMB);
	if (rc) {
		cifs_dbg(FYI, "Send error in SetFileInfo (SetFileSize) = %d\n",
			 rc);
	}

	/* Note: On -EAGAIN error only caller can retry on handle based calls
		since file handle passed in no longer valid */

	return rc;
}

int
SMBSetInformation(const unsigned int xid, struct cifs_tcon *tcon,
		  const char *fileName, __le32 attributes, __le64 write_time,
		  const struct nls_table *nls_codepage,
		  struct cifs_sb_info *cifs_sb)
{
	SETATTR_REQ *pSMB;
	SETATTR_RSP *pSMBr;
	struct timespec64 ts;
	int bytes_returned;
	int name_len;
	int rc;

	cifs_dbg(FYI, "In %s path %s\n", __func__, fileName);

retry:
	rc = smb_init(SMB_COM_SETATTR, 8, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
			cifsConvertToUTF16((__le16 *) pSMB->fileName,
					   fileName, PATH_MAX, nls_codepage,
					   cifs_remap(cifs_sb));
		name_len++;     /* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->fileName, fileName);
	}
	/* Only few attributes can be set by this command, others are not accepted by Win9x. */
	pSMB->attr = cpu_to_le16(le32_to_cpu(attributes) &
			(ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE));
	/* Zero write time value (in both NT and SETATTR formats) means to not change it. */
	if (le64_to_cpu(write_time) != 0) {
		ts = cifs_NTtimeToUnix(write_time);
		pSMB->last_write_time = cpu_to_le32(ts.tv_sec);
	}
	pSMB->BufferFormat = 0x04;
	name_len++; /* account for buffer type byte */
	inc_rfc1001_len(pSMB, (__u16)name_len);
	pSMB->ByteCount = cpu_to_le16(name_len);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc)
		cifs_dbg(FYI, "Send error in %s = %d\n", __func__, rc);

	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto retry;

	return rc;
}

/* Some legacy servers such as NT4 require that the file times be set on
   an open handle, rather than by pathname - this is awkward due to
   potential access conflicts on the open, but it is unavoidable for these
   old servers since the only other choice is to go from 100 nanosecond DCE
   time and resort to the original setpathinfo level which takes the ancient
   DOS time format with 2 second granularity */
int
CIFSSMBSetFileInfo(const unsigned int xid, struct cifs_tcon *tcon,
		    const FILE_BASIC_INFO *data, __u16 fid, __u32 pid_of_opener)
{
	struct smb_com_transaction2_sfi_req *pSMB  = NULL;
	char *data_offset;
	int rc = 0;
	__u16 params, param_offset, offset, byte_count, count;

	cifs_dbg(FYI, "Set Times (via SetFileInfo)\n");
	rc = small_smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB);

	if (rc)
		return rc;

	pSMB->hdr.Pid = cpu_to_le16((__u16)pid_of_opener);
	pSMB->hdr.PidHigh = cpu_to_le16((__u16)(pid_of_opener >> 16));

	params = 6;
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_sfi_req, Fid) - 4;
	offset = param_offset + params;

	data_offset = (char *)pSMB +
			offsetof(struct smb_hdr, Protocol) + offset;

	count = sizeof(FILE_BASIC_INFO);
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find max SMB PDU from sess */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_FILE_INFORMATION);
	byte_count = 3 /* pad */  + params + count;
	pSMB->DataCount = cpu_to_le16(count);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->Fid = fid;
	if (tcon->ses->capabilities & CAP_INFOLEVEL_PASSTHRU)
		pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_BASIC_INFO2);
	else
		pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_BASIC_INFO);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	memcpy(data_offset, data, sizeof(FILE_BASIC_INFO));
	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) pSMB, 0);
	cifs_small_buf_release(pSMB);
	if (rc)
		cifs_dbg(FYI, "Send error in Set Time (SetFileInfo) = %d\n",
			 rc);

	/* Note: On -EAGAIN error only caller can retry on handle based calls
		since file handle passed in no longer valid */

	return rc;
}

int
CIFSSMBSetFileDisposition(const unsigned int xid, struct cifs_tcon *tcon,
			  bool delete_file, __u16 fid, __u32 pid_of_opener)
{
	struct smb_com_transaction2_sfi_req *pSMB  = NULL;
	char *data_offset;
	int rc = 0;
	__u16 params, param_offset, offset, byte_count, count;

	cifs_dbg(FYI, "Set File Disposition (via SetFileInfo)\n");
	rc = small_smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB);

	if (rc)
		return rc;

	pSMB->hdr.Pid = cpu_to_le16((__u16)pid_of_opener);
	pSMB->hdr.PidHigh = cpu_to_le16((__u16)(pid_of_opener >> 16));

	params = 6;
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_sfi_req, Fid) - 4;
	offset = param_offset + params;

	/* SMB offsets are from the beginning of SMB which is 4 bytes in, after RFC1001 field */
	data_offset = (char *)(pSMB) + offset + 4;

	count = 1;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find max SMB PDU from sess */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_FILE_INFORMATION);
	byte_count = 3 /* pad */  + params + count;
	pSMB->DataCount = cpu_to_le16(count);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->Fid = fid;
	pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_DISPOSITION_INFO);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	*data_offset = delete_file ? 1 : 0;
	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) pSMB, 0);
	cifs_small_buf_release(pSMB);
	if (rc)
		cifs_dbg(FYI, "Send error in SetFileDisposition = %d\n", rc);

	return rc;
}

static int
CIFSSMBSetPathInfoFB(const unsigned int xid, struct cifs_tcon *tcon,
		     const char *fileName, const FILE_BASIC_INFO *data,
		     const struct nls_table *nls_codepage,
		     struct cifs_sb_info *cifs_sb)
{
	int oplock = 0;
	struct cifs_open_parms oparms;
	struct cifs_fid fid;
	int rc;

	oparms = (struct cifs_open_parms) {
		.tcon = tcon,
		.cifs_sb = cifs_sb,
		.desired_access = GENERIC_WRITE,
		.create_options = cifs_create_options(cifs_sb, 0),
		.disposition = FILE_OPEN,
		.path = fileName,
		.fid = &fid,
	};

	rc = CIFS_open(xid, &oparms, &oplock, NULL);
	if (rc)
		goto out;

	rc = CIFSSMBSetFileInfo(xid, tcon, data, fid.netfid, current->tgid);
	CIFSSMBClose(xid, tcon, fid.netfid);
out:

	return rc;
}

int
CIFSSMBSetPathInfo(const unsigned int xid, struct cifs_tcon *tcon,
		   const char *fileName, const FILE_BASIC_INFO *data,
		   const struct nls_table *nls_codepage,
		     struct cifs_sb_info *cifs_sb)
{
	TRANSACTION2_SPI_REQ *pSMB = NULL;
	TRANSACTION2_SPI_RSP *pSMBr = NULL;
	int name_len;
	int rc = 0;
	int bytes_returned = 0;
	char *data_offset;
	__u16 params, param_offset, offset, byte_count, count;
	int remap = cifs_remap(cifs_sb);

	cifs_dbg(FYI, "In SetTimes\n");

SetTimesRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, fileName,
				       PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, fileName);
	}

	params = 6 + name_len;
	count = sizeof(FILE_BASIC_INFO);
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;
	data_offset = (char *)pSMB + offsetof(typeof(*pSMB), hdr.Protocol) + offset;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	byte_count = 3 /* pad */  + params + count;

	pSMB->DataCount = cpu_to_le16(count);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	if (tcon->ses->capabilities & CAP_INFOLEVEL_PASSTHRU)
		pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_BASIC_INFO2);
	else
		pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_BASIC_INFO);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	memcpy(data_offset, data, sizeof(FILE_BASIC_INFO));
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc)
		cifs_dbg(FYI, "SetPathInfo (times) returned %d\n", rc);

	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto SetTimesRetry;

	if (rc == -EOPNOTSUPP)
		return CIFSSMBSetPathInfoFB(xid, tcon, fileName, data,
					    nls_codepage, cifs_sb);

	return rc;
}

static void
cifs_fill_unix_set_info(FILE_UNIX_BASIC_INFO *data_offset,
			const struct cifs_unix_set_info_args *args)
{
	u64 uid = NO_CHANGE_64, gid = NO_CHANGE_64;
	u64 mode = args->mode;

	if (uid_valid(args->uid))
		uid = from_kuid(&init_user_ns, args->uid);
	if (gid_valid(args->gid))
		gid = from_kgid(&init_user_ns, args->gid);

	/*
	 * Samba server ignores set of file size to zero due to bugs in some
	 * older clients, but we should be precise - we use SetFileSize to
	 * set file size and do not want to truncate file size to zero
	 * accidentally as happened on one Samba server beta by putting
	 * zero instead of -1 here
	 */
	data_offset->EndOfFile = cpu_to_le64(NO_CHANGE_64);
	data_offset->NumOfBytes = cpu_to_le64(NO_CHANGE_64);
	data_offset->LastStatusChange = cpu_to_le64(args->ctime);
	data_offset->LastAccessTime = cpu_to_le64(args->atime);
	data_offset->LastModificationTime = cpu_to_le64(args->mtime);
	data_offset->Uid = cpu_to_le64(uid);
	data_offset->Gid = cpu_to_le64(gid);
	/* better to leave device as zero when it is  */
	data_offset->DevMajor = cpu_to_le64(MAJOR(args->device));
	data_offset->DevMinor = cpu_to_le64(MINOR(args->device));
	data_offset->Permissions = cpu_to_le64(mode);

	if (S_ISREG(mode))
		data_offset->Type = cpu_to_le32(UNIX_FILE);
	else if (S_ISDIR(mode))
		data_offset->Type = cpu_to_le32(UNIX_DIR);
	else if (S_ISLNK(mode))
		data_offset->Type = cpu_to_le32(UNIX_SYMLINK);
	else if (S_ISCHR(mode))
		data_offset->Type = cpu_to_le32(UNIX_CHARDEV);
	else if (S_ISBLK(mode))
		data_offset->Type = cpu_to_le32(UNIX_BLOCKDEV);
	else if (S_ISFIFO(mode))
		data_offset->Type = cpu_to_le32(UNIX_FIFO);
	else if (S_ISSOCK(mode))
		data_offset->Type = cpu_to_le32(UNIX_SOCKET);
}

int
CIFSSMBUnixSetFileInfo(const unsigned int xid, struct cifs_tcon *tcon,
		       const struct cifs_unix_set_info_args *args,
		       u16 fid, u32 pid_of_opener)
{
	struct smb_com_transaction2_sfi_req *pSMB  = NULL;
	char *data_offset;
	int rc = 0;
	u16 params, param_offset, offset, byte_count, count;

	cifs_dbg(FYI, "Set Unix Info (via SetFileInfo)\n");
	rc = small_smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB);

	if (rc)
		return rc;

	pSMB->hdr.Pid = cpu_to_le16((__u16)pid_of_opener);
	pSMB->hdr.PidHigh = cpu_to_le16((__u16)(pid_of_opener >> 16));

	params = 6;
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_sfi_req, Fid) - 4;
	offset = param_offset + params;

	data_offset = (char *)pSMB +
			offsetof(struct smb_hdr, Protocol) + offset;

	count = sizeof(FILE_UNIX_BASIC_INFO);

	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find max SMB PDU from sess */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_FILE_INFORMATION);
	byte_count = 3 /* pad */  + params + count;
	pSMB->DataCount = cpu_to_le16(count);
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->Fid = fid;
	pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_UNIX_BASIC);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	cifs_fill_unix_set_info((FILE_UNIX_BASIC_INFO *)data_offset, args);

	rc = SendReceiveNoRsp(xid, tcon->ses, (char *) pSMB, 0);
	cifs_small_buf_release(pSMB);
	if (rc)
		cifs_dbg(FYI, "Send error in Set Time (SetFileInfo) = %d\n",
			 rc);

	/* Note: On -EAGAIN error only caller can retry on handle based calls
		since file handle passed in no longer valid */

	return rc;
}

int
CIFSSMBUnixSetPathInfo(const unsigned int xid, struct cifs_tcon *tcon,
		       const char *file_name,
		       const struct cifs_unix_set_info_args *args,
		       const struct nls_table *nls_codepage, int remap)
{
	TRANSACTION2_SPI_REQ *pSMB = NULL;
	TRANSACTION2_SPI_RSP *pSMBr = NULL;
	int name_len;
	int rc = 0;
	int bytes_returned = 0;
	FILE_UNIX_BASIC_INFO *data_offset;
	__u16 params, param_offset, offset, count, byte_count;

	cifs_dbg(FYI, "In SetUID/GID/Mode\n");
setPermsRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, file_name,
				       PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, file_name);
	}

	params = 6 + name_len;
	count = sizeof(FILE_UNIX_BASIC_INFO);
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;
	/* SMB offsets are from the beginning of SMB which is 4 bytes in, after RFC1001 field */
	data_offset = (FILE_UNIX_BASIC_INFO *)((char *) pSMB + offset + 4);
	memset(data_offset, 0, count);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	byte_count = 3 /* pad */  + params + count;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->DataCount = cpu_to_le16(count);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_SET_FILE_UNIX_BASIC);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);

	cifs_fill_unix_set_info(data_offset, args);

	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc)
		cifs_dbg(FYI, "SetPathInfo (perms) returned %d\n", rc);

	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto setPermsRetry;
	return rc;
}

#ifdef CONFIG_CIFS_XATTR
/*
 * Do a path-based QUERY_ALL_EAS call and parse the result. This is a common
 * function used by listxattr and getxattr type calls. When ea_name is set,
 * it looks for that attribute name and stuffs that value into the EAData
 * buffer. When ea_name is NULL, it stuffs a list of attribute names into the
 * buffer. In both cases, the return value is either the length of the
 * resulting data or a negative error code. If EAData is a NULL pointer then
 * the data isn't copied to it, but the length is returned.
 */
ssize_t
CIFSSMBQAllEAs(const unsigned int xid, struct cifs_tcon *tcon,
		const unsigned char *searchName, const unsigned char *ea_name,
		char *EAData, size_t buf_size,
		struct cifs_sb_info *cifs_sb)
{
		/* BB assumes one setup word */
	TRANSACTION2_QPI_REQ *pSMB = NULL;
	TRANSACTION2_QPI_RSP *pSMBr = NULL;
	int remap = cifs_remap(cifs_sb);
	struct nls_table *nls_codepage = cifs_sb->local_nls;
	int rc = 0;
	int bytes_returned;
	int list_len;
	struct fealist *ea_response_data;
	struct fea *temp_fea;
	char *temp_ptr;
	char *end_of_smb;
	__u16 params, byte_count, data_offset;
	unsigned int ea_name_len = ea_name ? strlen(ea_name) : 0;

	cifs_dbg(FYI, "In Query All EAs path %s\n", searchName);
QAllEAsRetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		list_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, searchName,
				       PATH_MAX, nls_codepage, remap);
		list_len++;	/* trailing null */
		list_len *= 2;
	} else {
		list_len = copy_path_name(pSMB->FileName, searchName);
	}

	params = 2 /* level */ + 4 /* reserved */ + list_len /* includes NUL */;
	pSMB->TotalDataCount = 0;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find exact max SMB PDU from sess structure BB */
	pSMB->MaxDataCount = cpu_to_le16(CIFSMaxBufSize);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	pSMB->ParameterOffset = cpu_to_le16(offsetof(
	struct smb_com_transaction2_qpi_req, InformationLevel) - 4);
	pSMB->DataCount = 0;
	pSMB->DataOffset = 0;
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_QUERY_PATH_INFORMATION);
	byte_count = params + 1 /* pad */ ;
	pSMB->TotalParameterCount = cpu_to_le16(params);
	pSMB->ParameterCount = pSMB->TotalParameterCount;
	pSMB->InformationLevel = cpu_to_le16(SMB_INFO_QUERY_ALL_EAS);
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);

	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc) {
		cifs_dbg(FYI, "Send error in QueryAllEAs = %d\n", rc);
		goto QAllEAsOut;
	}


	/* BB also check enough total bytes returned */
	/* BB we need to improve the validity checking
	of these trans2 responses */

	rc = validate_t2((struct smb_t2_rsp *)pSMBr);
	if (rc || get_bcc(&pSMBr->hdr) < 4) {
		rc = -EIO;	/* bad smb */
		goto QAllEAsOut;
	}

	/* check that length of list is not more than bcc */
	/* check that each entry does not go beyond length
	   of list */
	/* check that each element of each entry does not
	   go beyond end of list */
	/* validate_trans2_offsets() */
	/* BB check if start of smb + data_offset > &bcc+ bcc */

	data_offset = le16_to_cpu(pSMBr->t2.DataOffset);
	ea_response_data = (struct fealist *)
				(((char *) &pSMBr->hdr.Protocol) + data_offset);

	list_len = le32_to_cpu(ea_response_data->list_len);
	cifs_dbg(FYI, "ea length %d\n", list_len);
	if (list_len <= 8) {
		cifs_dbg(FYI, "empty EA list returned from server\n");
		/* didn't find the named attribute */
		if (ea_name)
			rc = -ENODATA;
		goto QAllEAsOut;
	}

	/* make sure list_len doesn't go past end of SMB */
	end_of_smb = (char *)pByteArea(&pSMBr->hdr) + get_bcc(&pSMBr->hdr);
	if ((char *)ea_response_data + list_len > end_of_smb) {
		cifs_dbg(FYI, "EA list appears to go beyond SMB\n");
		rc = -EIO;
		goto QAllEAsOut;
	}

	/* account for ea list len */
	list_len -= 4;
	temp_fea = &ea_response_data->list;
	temp_ptr = (char *)temp_fea;
	while (list_len > 0) {
		unsigned int name_len;
		__u16 value_len;

		list_len -= 4;
		temp_ptr += 4;
		/* make sure we can read name_len and value_len */
		if (list_len < 0) {
			cifs_dbg(FYI, "EA entry goes beyond length of list\n");
			rc = -EIO;
			goto QAllEAsOut;
		}

		name_len = temp_fea->name_len;
		value_len = le16_to_cpu(temp_fea->value_len);
		list_len -= name_len + 1 + value_len;
		if (list_len < 0) {
			cifs_dbg(FYI, "EA entry goes beyond length of list\n");
			rc = -EIO;
			goto QAllEAsOut;
		}

		if (ea_name) {
			if (ea_name_len == name_len &&
			    memcmp(ea_name, temp_ptr, name_len) == 0) {
				temp_ptr += name_len + 1;
				rc = value_len;
				if (buf_size == 0)
					goto QAllEAsOut;
				if ((size_t)value_len > buf_size) {
					rc = -ERANGE;
					goto QAllEAsOut;
				}
				memcpy(EAData, temp_ptr, value_len);
				goto QAllEAsOut;
			}
		} else {
			/* account for prefix user. and trailing null */
			rc += (5 + 1 + name_len);
			if (rc < (int) buf_size) {
				memcpy(EAData, "user.", 5);
				EAData += 5;
				memcpy(EAData, temp_ptr, name_len);
				EAData += name_len;
				/* null terminate name */
				*EAData = 0;
				++EAData;
			} else if (buf_size == 0) {
				/* skip copy - calc size only */
			} else {
				/* stop before overrun buffer */
				rc = -ERANGE;
				break;
			}
		}
		temp_ptr += name_len + 1 + value_len;
		temp_fea = (struct fea *)temp_ptr;
	}

	/* didn't find the named attribute */
	if (ea_name)
		rc = -ENODATA;

QAllEAsOut:
	cifs_buf_release(pSMB);
	if (rc == -EAGAIN)
		goto QAllEAsRetry;

	return (ssize_t)rc;
}

int
CIFSSMBSetEA(const unsigned int xid, struct cifs_tcon *tcon,
	     const char *fileName, const char *ea_name, const void *ea_value,
	     const __u16 ea_value_len, const struct nls_table *nls_codepage,
	     struct cifs_sb_info *cifs_sb)
{
	struct smb_com_transaction2_spi_req *pSMB = NULL;
	struct smb_com_transaction2_spi_rsp *pSMBr = NULL;
	struct fealist *parm_data;
	int name_len;
	int rc = 0;
	int bytes_returned = 0;
	__u16 params, param_offset, byte_count, offset, count;
	int remap = cifs_remap(cifs_sb);

	cifs_dbg(FYI, "In SetEA\n");
SetEARetry:
	rc = smb_init(SMB_COM_TRANSACTION2, 15, tcon, (void **) &pSMB,
		      (void **) &pSMBr);
	if (rc)
		return rc;

	if (pSMB->hdr.Flags2 & SMBFLG2_UNICODE) {
		name_len =
		    cifsConvertToUTF16((__le16 *) pSMB->FileName, fileName,
				       PATH_MAX, nls_codepage, remap);
		name_len++;	/* trailing null */
		name_len *= 2;
	} else {
		name_len = copy_path_name(pSMB->FileName, fileName);
	}

	params = 6 + name_len;

	/* done calculating parms using name_len of file name,
	now use name_len to calculate length of ea name
	we are going to create in the inode xattrs */
	if (ea_name == NULL)
		name_len = 0;
	else
		name_len = strnlen(ea_name, 255);

	count = sizeof(*parm_data) + 1 + ea_value_len + name_len;
	pSMB->MaxParameterCount = cpu_to_le16(2);
	/* BB find max SMB PDU from sess */
	pSMB->MaxDataCount = cpu_to_le16(1000);
	pSMB->MaxSetupCount = 0;
	pSMB->Reserved = 0;
	pSMB->Flags = 0;
	pSMB->Timeout = 0;
	pSMB->Reserved2 = 0;
	param_offset = offsetof(struct smb_com_transaction2_spi_req,
				InformationLevel) - 4;
	offset = param_offset + params;
	pSMB->InformationLevel =
		cpu_to_le16(SMB_SET_FILE_EA);

	parm_data = (void *)pSMB + offsetof(struct smb_hdr, Protocol) + offset;
	pSMB->ParameterOffset = cpu_to_le16(param_offset);
	pSMB->DataOffset = cpu_to_le16(offset);
	pSMB->SetupCount = 1;
	pSMB->Reserved3 = 0;
	pSMB->SubCommand = cpu_to_le16(TRANS2_SET_PATH_INFORMATION);
	byte_count = 3 /* pad */  + params + count;
	pSMB->DataCount = cpu_to_le16(count);
	parm_data->list_len = cpu_to_le32(count);
	parm_data->list.EA_flags = 0;
	/* we checked above that name len is less than 255 */
	parm_data->list.name_len = (__u8)name_len;
	/* EA names are always ASCII and NUL-terminated */
	strscpy(parm_data->list.name, ea_name ?: "", name_len + 1);
	parm_data->list.value_len = cpu_to_le16(ea_value_len);
	/* caller ensures that ea_value_len is less than 64K but
	we need to ensure that it fits within the smb */

	/*BB add length check to see if it would fit in
	     negotiated SMB buffer size BB */
	/* if (ea_value_len > buffer_size - 512 (enough for header)) */
	if (ea_value_len)
		memcpy(parm_data->list.name + name_len + 1,
		       ea_value, ea_value_len);

	pSMB->TotalDataCount = pSMB->DataCount;
	pSMB->ParameterCount = cpu_to_le16(params);
	pSMB->TotalParameterCount = pSMB->ParameterCount;
	pSMB->Reserved4 = 0;
	inc_rfc1001_len(pSMB, byte_count);
	pSMB->ByteCount = cpu_to_le16(byte_count);
	rc = SendReceive(xid, tcon->ses, (struct smb_hdr *) pSMB,
			 (struct smb_hdr *) pSMBr, &bytes_returned, 0);
	if (rc)
		cifs_dbg(FYI, "SetPathInfo (EA) returned %d\n", rc);

	cifs_buf_release(pSMB);

	if (rc == -EAGAIN)
		goto SetEARetry;

	return rc;
}
#endif
