/************************************************************

Author: Eamon Walsh <ewalsh@tycho.nsa.gov>

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
this permission notice appear in supporting documentation.  This permission
notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

********************************************************/

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "selection.h"
#include "inputstr.h"
#include "windowstr.h"
#include "propertyst.h"
#include "extnsionst.h"
#include "modinit.h"
#include "xselinuxint.h"

#define CTX_DEV offsetof(SELinuxSubjectRec, dev_create_sid)
#define CTX_WIN offsetof(SELinuxSubjectRec, win_create_sid)
#define CTX_PRP offsetof(SELinuxSubjectRec, prp_create_sid)
#define CTX_SEL offsetof(SELinuxSubjectRec, sel_create_sid)
#define USE_PRP offsetof(SELinuxSubjectRec, prp_use_sid)
#define USE_SEL offsetof(SELinuxSubjectRec, sel_use_sid)

typedef struct {
    security_context_t octx;
    security_context_t dctx;
    CARD32 octx_len;
    CARD32 dctx_len;
    CARD32 id;
} SELinuxListItemRec;


/*
 * Extension Dispatch
 */

static security_context_t
SELinuxCopyContext(char *ptr, unsigned len)
{
    security_context_t copy = xalloc(len + 1);
    if (!copy)
	return NULL;
    strncpy(copy, ptr, len);
    copy[len] = '\0';
    return copy;
}

static int
ProcSELinuxQueryVersion(ClientPtr client)
{
    SELinuxQueryVersionReply rep;

    rep.type = X_Reply;
    rep.length = 0;
    rep.sequenceNumber = client->sequence;
    rep.server_major = SELINUX_MAJOR_VERSION;
    rep.server_minor = SELINUX_MINOR_VERSION;
    if (client->swapped) {
	int n;
	swaps(&rep.sequenceNumber, n);
	swapl(&rep.length, n);
	swaps(&rep.server_major, n);
	swaps(&rep.server_minor, n);
    }
    WriteToClient(client, sizeof(rep), (char *)&rep);
    return (client->noClientException);
}

static int
SELinuxSendContextReply(ClientPtr client, security_id_t sid)
{
    SELinuxGetContextReply rep;
    security_context_t ctx = NULL;
    int len = 0;

    if (sid) {
	if (avc_sid_to_context_raw(sid, &ctx) < 0)
	    return BadValue;
	len = strlen(ctx) + 1;
    }

    rep.type = X_Reply;
    rep.length = bytes_to_int32(len);
    rep.sequenceNumber = client->sequence;
    rep.context_len = len;

    if (client->swapped) {
	int n;
	swapl(&rep.length, n);
	swaps(&rep.sequenceNumber, n);
	swapl(&rep.context_len, n);
    }

    WriteToClient(client, sizeof(SELinuxGetContextReply), (char *)&rep);
    WriteToClient(client, len, ctx);
    freecon(ctx);
    return client->noClientException;
}

static int
ProcSELinuxSetCreateContext(ClientPtr client, unsigned offset)
{
    PrivateRec **privPtr = &client->devPrivates;
    security_id_t *pSid;
    security_context_t ctx = NULL;
    char *ptr;
    int rc;

    REQUEST(SELinuxSetCreateContextReq);
    REQUEST_FIXED_SIZE(SELinuxSetCreateContextReq, stuff->context_len);

    if (stuff->context_len > 0) {
	ctx = SELinuxCopyContext((char *)(stuff + 1), stuff->context_len);
	if (!ctx)
	    return BadAlloc;
    }

    ptr = dixLookupPrivate(privPtr, subjectKey);
    pSid = (security_id_t *)(ptr + offset);
    sidput(*pSid);
    *pSid = NULL;

    rc = Success;
    if (stuff->context_len > 0) {
	if (security_check_context_raw(ctx) < 0 ||
	    avc_context_to_sid_raw(ctx, pSid) < 0)
	    rc = BadValue;
    }

    xfree(ctx);
    return rc;
}

static int
ProcSELinuxGetCreateContext(ClientPtr client, unsigned offset)
{
    security_id_t *pSid;
    char *ptr;

    REQUEST_SIZE_MATCH(SELinuxGetCreateContextReq);

    if (offset == CTX_DEV)
	ptr = dixLookupPrivate(&serverClient->devPrivates, subjectKey);
    else
	ptr = dixLookupPrivate(&client->devPrivates, subjectKey);

    pSid = (security_id_t *)(ptr + offset);
    return SELinuxSendContextReply(client, *pSid);
}

static int
ProcSELinuxSetDeviceContext(ClientPtr client)
{
    security_context_t ctx;
    security_id_t sid;
    DeviceIntPtr dev;
    SELinuxSubjectRec *subj;
    SELinuxObjectRec *obj;
    int rc;

    REQUEST(SELinuxSetContextReq);
    REQUEST_FIXED_SIZE(SELinuxSetContextReq, stuff->context_len);

    if (stuff->context_len < 1)
	return BadLength;
    ctx = SELinuxCopyContext((char *)(stuff + 1), stuff->context_len);
    if (!ctx)
	return BadAlloc;

    rc = dixLookupDevice(&dev, stuff->id, client, DixManageAccess);
    if (rc != Success)
	goto out;

    if (security_check_context_raw(ctx) < 0 ||
	avc_context_to_sid_raw(ctx, &sid) < 0) {
	rc = BadValue;
	goto out;
    }

    subj = dixLookupPrivate(&dev->devPrivates, subjectKey);
    sidput(subj->sid);
    subj->sid = sid;
    obj = dixLookupPrivate(&dev->devPrivates, objectKey);
    sidput(obj->sid);
    sidget(obj->sid = sid);

    rc = Success;
out:
    xfree(ctx);
    return rc;
}

static int
ProcSELinuxGetDeviceContext(ClientPtr client)
{
    DeviceIntPtr dev;
    SELinuxSubjectRec *subj;
    int rc;

    REQUEST(SELinuxGetContextReq);
    REQUEST_SIZE_MATCH(SELinuxGetContextReq);

    rc = dixLookupDevice(&dev, stuff->id, client, DixGetAttrAccess);
    if (rc != Success)
	return rc;

    subj = dixLookupPrivate(&dev->devPrivates, subjectKey);
    return SELinuxSendContextReply(client, subj->sid);
}

static int
ProcSELinuxGetWindowContext(ClientPtr client)
{
    WindowPtr pWin;
    SELinuxObjectRec *obj;
    int rc;

    REQUEST(SELinuxGetContextReq);
    REQUEST_SIZE_MATCH(SELinuxGetContextReq);

    rc = dixLookupWindow(&pWin, stuff->id, client, DixGetAttrAccess);
    if (rc != Success)
	return rc;

    obj = dixLookupPrivate(&pWin->devPrivates, objectKey);
    return SELinuxSendContextReply(client, obj->sid);
}

static int
ProcSELinuxGetPropertyContext(ClientPtr client, pointer privKey)
{
    WindowPtr pWin;
    PropertyPtr pProp;
    SELinuxObjectRec *obj;
    int rc;

    REQUEST(SELinuxGetPropertyContextReq);
    REQUEST_SIZE_MATCH(SELinuxGetPropertyContextReq);

    rc = dixLookupWindow(&pWin, stuff->window, client, DixGetPropAccess);
    if (rc != Success)
	return rc;

    rc = dixLookupProperty(&pProp, pWin, stuff->property, client,
			   DixGetAttrAccess);
    if (rc != Success)
	return rc;

    obj = dixLookupPrivate(&pProp->devPrivates, privKey);
    return SELinuxSendContextReply(client, obj->sid);
}

static int
ProcSELinuxGetSelectionContext(ClientPtr client, pointer privKey)
{
    Selection *pSel;
    SELinuxObjectRec *obj;
    int rc;

    REQUEST(SELinuxGetContextReq);
    REQUEST_SIZE_MATCH(SELinuxGetContextReq);

    rc = dixLookupSelection(&pSel, stuff->id, client, DixGetAttrAccess);
    if (rc != Success)
	return rc;

    obj = dixLookupPrivate(&pSel->devPrivates, privKey);
    return SELinuxSendContextReply(client, obj->sid);
}

static int
ProcSELinuxGetClientContext(ClientPtr client)
{
    ClientPtr target;
    SELinuxSubjectRec *subj;
    int rc;

    REQUEST(SELinuxGetContextReq);
    REQUEST_SIZE_MATCH(SELinuxGetContextReq);

    rc = dixLookupClient(&target, stuff->id, client, DixGetAttrAccess);
    if (rc != Success)
	return rc;

    subj = dixLookupPrivate(&target->devPrivates, subjectKey);
    return SELinuxSendContextReply(client, subj->sid);
}

static int
SELinuxPopulateItem(SELinuxListItemRec *i, PrivateRec **privPtr, CARD32 id,
		    int *size)
{
    SELinuxObjectRec *obj = dixLookupPrivate(privPtr, objectKey);
    SELinuxObjectRec *data = dixLookupPrivate(privPtr, dataKey);

    if (avc_sid_to_context_raw(obj->sid, &i->octx) < 0)
	return BadValue;
    if (avc_sid_to_context_raw(data->sid, &i->dctx) < 0)
	return BadValue;

    i->id = id;
    i->octx_len = bytes_to_int32(strlen(i->octx) + 1);
    i->dctx_len = bytes_to_int32(strlen(i->dctx) + 1);

    *size += i->octx_len + i->dctx_len + 3;
    return Success;
}

static void
SELinuxFreeItems(SELinuxListItemRec *items, int count)
{
    int k;
    for (k = 0; k < count; k++) {
	freecon(items[k].octx);
	freecon(items[k].dctx);
    }
    xfree(items);
}

static int
SELinuxSendItemsToClient(ClientPtr client, SELinuxListItemRec *items,
			 int size, int count)
{
    int rc, k, n, pos = 0;
    SELinuxListItemsReply rep;
    CARD32 *buf;

    buf = xcalloc(size, sizeof(CARD32));
    if (size && !buf) {
	rc = BadAlloc;
	goto out;
    }

    /* Fill in the buffer */
    for (k = 0; k < count; k++) {
	buf[pos] = items[k].id;
	if (client->swapped)
	    swapl(buf + pos, n);
	pos++;

	buf[pos] = items[k].octx_len * 4;
	if (client->swapped)
	    swapl(buf + pos, n);
	pos++;

	buf[pos] = items[k].dctx_len * 4;
	if (client->swapped)
	    swapl(buf + pos, n);
	pos++;

	memcpy((char *)(buf + pos), items[k].octx, strlen(items[k].octx) + 1);
	pos += items[k].octx_len;
	memcpy((char *)(buf + pos), items[k].dctx, strlen(items[k].dctx) + 1);
	pos += items[k].dctx_len;
    }

    /* Send reply to client */
    rep.type = X_Reply;
    rep.length = size;
    rep.sequenceNumber = client->sequence;
    rep.count = count;

    if (client->swapped) {
	swapl(&rep.length, n);
	swaps(&rep.sequenceNumber, n);
	swapl(&rep.count, n);
    }

    WriteToClient(client, sizeof(SELinuxListItemsReply), (char *)&rep);
    WriteToClient(client, size * 4, (char *)buf);

    /* Free stuff and return */
    rc = client->noClientException;
    xfree(buf);
out:
    SELinuxFreeItems(items, count);
    return rc;
}

static int
ProcSELinuxListProperties(ClientPtr client)
{
    WindowPtr pWin;
    PropertyPtr pProp;
    SELinuxListItemRec *items;
    int rc, count, size, i;
    CARD32 id;

    REQUEST(SELinuxGetContextReq);
    REQUEST_SIZE_MATCH(SELinuxGetContextReq);

    rc = dixLookupWindow(&pWin, stuff->id, client, DixListPropAccess);
    if (rc != Success)
	return rc;

    /* Count the number of properties and allocate items */
    count = 0;
    for (pProp = wUserProps(pWin); pProp; pProp = pProp->next)
	count++;
    items = xcalloc(count, sizeof(SELinuxListItemRec));
    if (count && !items)
	return BadAlloc;

    /* Fill in the items and calculate size */
    i = 0;
    size = 0;
    for (pProp = wUserProps(pWin); pProp; pProp = pProp->next) {
	id = pProp->propertyName;
	rc = SELinuxPopulateItem(items + i, &pProp->devPrivates, id, &size);
	if (rc != Success) {
	    SELinuxFreeItems(items, count);
	    return rc;
	}
	i++;
    }

    return SELinuxSendItemsToClient(client, items, size, count);
}

static int
ProcSELinuxListSelections(ClientPtr client)
{
    Selection *pSel;
    SELinuxListItemRec *items;
    int rc, count, size, i;
    CARD32 id;

    REQUEST_SIZE_MATCH(SELinuxGetCreateContextReq);

    /* Count the number of selections and allocate items */
    count = 0;
    for (pSel = CurrentSelections; pSel; pSel = pSel->next)
	count++;
    items = xcalloc(count, sizeof(SELinuxListItemRec));
    if (count && !items)
	return BadAlloc;

    /* Fill in the items and calculate size */
    i = 0;
    size = 0;
    for (pSel = CurrentSelections; pSel; pSel = pSel->next) {
	id = pSel->selection;
	rc = SELinuxPopulateItem(items + i, &pSel->devPrivates, id, &size);
	if (rc != Success) {
	    SELinuxFreeItems(items, count);
	    return rc;
	}
	i++;
    }

    return SELinuxSendItemsToClient(client, items, size, count);
}

static int
ProcSELinuxDispatch(ClientPtr client)
{
    REQUEST(xReq);
    switch (stuff->data) {
    case X_SELinuxQueryVersion:
	return ProcSELinuxQueryVersion(client);
    case X_SELinuxSetDeviceCreateContext:
	return ProcSELinuxSetCreateContext(client, CTX_DEV);
    case X_SELinuxGetDeviceCreateContext:
	return ProcSELinuxGetCreateContext(client, CTX_DEV);
    case X_SELinuxSetDeviceContext:
	return ProcSELinuxSetDeviceContext(client);
    case X_SELinuxGetDeviceContext:
	return ProcSELinuxGetDeviceContext(client);
    case X_SELinuxSetWindowCreateContext:
	return ProcSELinuxSetCreateContext(client, CTX_WIN);
    case X_SELinuxGetWindowCreateContext:
	return ProcSELinuxGetCreateContext(client, CTX_WIN);
    case X_SELinuxGetWindowContext:
	return ProcSELinuxGetWindowContext(client);
    case X_SELinuxSetPropertyCreateContext:
	return ProcSELinuxSetCreateContext(client, CTX_PRP);
    case X_SELinuxGetPropertyCreateContext:
	return ProcSELinuxGetCreateContext(client, CTX_PRP);
    case X_SELinuxSetPropertyUseContext:
	return ProcSELinuxSetCreateContext(client, USE_PRP);
    case X_SELinuxGetPropertyUseContext:
	return ProcSELinuxGetCreateContext(client, USE_PRP);
    case X_SELinuxGetPropertyContext:
	return ProcSELinuxGetPropertyContext(client, objectKey);
    case X_SELinuxGetPropertyDataContext:
	return ProcSELinuxGetPropertyContext(client, dataKey);
    case X_SELinuxListProperties:
	return ProcSELinuxListProperties(client);
    case X_SELinuxSetSelectionCreateContext:
	return ProcSELinuxSetCreateContext(client, CTX_SEL);
    case X_SELinuxGetSelectionCreateContext:
	return ProcSELinuxGetCreateContext(client, CTX_SEL);
    case X_SELinuxSetSelectionUseContext:
	return ProcSELinuxSetCreateContext(client, USE_SEL);
    case X_SELinuxGetSelectionUseContext:
	return ProcSELinuxGetCreateContext(client, USE_SEL);
    case X_SELinuxGetSelectionContext:
	return ProcSELinuxGetSelectionContext(client, objectKey);
    case X_SELinuxGetSelectionDataContext:
	return ProcSELinuxGetSelectionContext(client, dataKey);
    case X_SELinuxListSelections:
	return ProcSELinuxListSelections(client);
    case X_SELinuxGetClientContext:
	return ProcSELinuxGetClientContext(client);
    default:
	return BadRequest;
    }
}

static int
SProcSELinuxQueryVersion(ClientPtr client)
{
    REQUEST(SELinuxQueryVersionReq);
    int n;

    REQUEST_SIZE_MATCH(SELinuxQueryVersionReq);
    swaps(&stuff->client_major, n);
    swaps(&stuff->client_minor, n);
    return ProcSELinuxQueryVersion(client);
}

static int
SProcSELinuxSetCreateContext(ClientPtr client, unsigned offset)
{
    REQUEST(SELinuxSetCreateContextReq);
    int n;

    REQUEST_AT_LEAST_SIZE(SELinuxSetCreateContextReq);
    swapl(&stuff->context_len, n);
    return ProcSELinuxSetCreateContext(client, offset);
}

static int
SProcSELinuxSetDeviceContext(ClientPtr client)
{
    REQUEST(SELinuxSetContextReq);
    int n;

    REQUEST_AT_LEAST_SIZE(SELinuxSetContextReq);
    swapl(&stuff->id, n);
    swapl(&stuff->context_len, n);
    return ProcSELinuxSetDeviceContext(client);
}

static int
SProcSELinuxGetDeviceContext(ClientPtr client)
{
    REQUEST(SELinuxGetContextReq);
    int n;

    REQUEST_SIZE_MATCH(SELinuxGetContextReq);
    swapl(&stuff->id, n);
    return ProcSELinuxGetDeviceContext(client);
}

static int
SProcSELinuxGetWindowContext(ClientPtr client)
{
    REQUEST(SELinuxGetContextReq);
    int n;

    REQUEST_SIZE_MATCH(SELinuxGetContextReq);
    swapl(&stuff->id, n);
    return ProcSELinuxGetWindowContext(client);
}

static int
SProcSELinuxGetPropertyContext(ClientPtr client, pointer privKey)
{
    REQUEST(SELinuxGetPropertyContextReq);
    int n;

    REQUEST_SIZE_MATCH(SELinuxGetPropertyContextReq);
    swapl(&stuff->window, n);
    swapl(&stuff->property, n);
    return ProcSELinuxGetPropertyContext(client, privKey);
}

static int
SProcSELinuxGetSelectionContext(ClientPtr client, pointer privKey)
{
    REQUEST(SELinuxGetContextReq);
    int n;

    REQUEST_SIZE_MATCH(SELinuxGetContextReq);
    swapl(&stuff->id, n);
    return ProcSELinuxGetSelectionContext(client, privKey);
}

static int
SProcSELinuxListProperties(ClientPtr client)
{
    REQUEST(SELinuxGetContextReq);
    int n;

    REQUEST_SIZE_MATCH(SELinuxGetContextReq);
    swapl(&stuff->id, n);
    return ProcSELinuxListProperties(client);
}

static int
SProcSELinuxGetClientContext(ClientPtr client)
{
    REQUEST(SELinuxGetContextReq);
    int n;

    REQUEST_SIZE_MATCH(SELinuxGetContextReq);
    swapl(&stuff->id, n);
    return ProcSELinuxGetClientContext(client);
}

static int
SProcSELinuxDispatch(ClientPtr client)
{
    REQUEST(xReq);
    int n;

    swaps(&stuff->length, n);

    switch (stuff->data) {
    case X_SELinuxQueryVersion:
	return SProcSELinuxQueryVersion(client);
    case X_SELinuxSetDeviceCreateContext:
	return SProcSELinuxSetCreateContext(client, CTX_DEV);
    case X_SELinuxGetDeviceCreateContext:
	return ProcSELinuxGetCreateContext(client, CTX_DEV);
    case X_SELinuxSetDeviceContext:
	return SProcSELinuxSetDeviceContext(client);
    case X_SELinuxGetDeviceContext:
	return SProcSELinuxGetDeviceContext(client);
    case X_SELinuxSetWindowCreateContext:
	return SProcSELinuxSetCreateContext(client, CTX_WIN);
    case X_SELinuxGetWindowCreateContext:
	return ProcSELinuxGetCreateContext(client, CTX_WIN);
    case X_SELinuxGetWindowContext:
	return SProcSELinuxGetWindowContext(client);
    case X_SELinuxSetPropertyCreateContext:
	return SProcSELinuxSetCreateContext(client, CTX_PRP);
    case X_SELinuxGetPropertyCreateContext:
	return ProcSELinuxGetCreateContext(client, CTX_PRP);
    case X_SELinuxSetPropertyUseContext:
	return SProcSELinuxSetCreateContext(client, USE_PRP);
    case X_SELinuxGetPropertyUseContext:
	return ProcSELinuxGetCreateContext(client, USE_PRP);
    case X_SELinuxGetPropertyContext:
	return SProcSELinuxGetPropertyContext(client, objectKey);
    case X_SELinuxGetPropertyDataContext:
	return SProcSELinuxGetPropertyContext(client, dataKey);
    case X_SELinuxListProperties:
	return SProcSELinuxListProperties(client);
    case X_SELinuxSetSelectionCreateContext:
	return SProcSELinuxSetCreateContext(client, CTX_SEL);
    case X_SELinuxGetSelectionCreateContext:
	return ProcSELinuxGetCreateContext(client, CTX_SEL);
    case X_SELinuxSetSelectionUseContext:
	return SProcSELinuxSetCreateContext(client, USE_SEL);
    case X_SELinuxGetSelectionUseContext:
	return ProcSELinuxGetCreateContext(client, USE_SEL);
    case X_SELinuxGetSelectionContext:
	return SProcSELinuxGetSelectionContext(client, objectKey);
    case X_SELinuxGetSelectionDataContext:
	return SProcSELinuxGetSelectionContext(client, dataKey);
    case X_SELinuxListSelections:
	return ProcSELinuxListSelections(client);
    case X_SELinuxGetClientContext:
	return SProcSELinuxGetClientContext(client);
    default:
	return BadRequest;
    }
}


/*
 * Extension Setup / Teardown
 */

static void
SELinuxResetProc(ExtensionEntry *extEntry)
{
    SELinuxFlaskReset();
    SELinuxLabelReset();
}

void
SELinuxExtensionInit(INITARGS)
{
    ExtensionEntry *extEntry;

    /* Check SELinux mode on system, configuration file, and boolean */
    if (!is_selinux_enabled()) {
	LogMessage(X_INFO, "SELinux: Disabled on system\n");
	return;
    }
    if (selinuxEnforcingState == SELINUX_MODE_DISABLED) {
	LogMessage(X_INFO, "SELinux: Disabled in configuration file\n");
	return;
    }
    if (!security_get_boolean_active("xserver_object_manager")) {
	LogMessage(X_INFO, "SELinux: Disabled by boolean\n");
        return;
    }

    /* Set up XACE hooks */
    SELinuxLabelInit();
    SELinuxFlaskInit();

    /* Add extension to server */
    extEntry = AddExtension(SELINUX_EXTENSION_NAME,
			    SELinuxNumberEvents, SELinuxNumberErrors,
			    ProcSELinuxDispatch, SProcSELinuxDispatch,
			    SELinuxResetProc, StandardMinorOpcode);

    AddExtensionAlias("Flask", extEntry);
}