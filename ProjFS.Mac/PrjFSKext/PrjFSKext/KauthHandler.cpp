#include <kern/debug.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <libkern/OSAtomic.h>
#include <kern/assert.h>
#include <stdatomic.h>

#include "PrjFSCommon.h"
#include "VirtualizationRoots.hpp"
#include "KauthHandler.hpp"
#include "KextLog.hpp"
#include "Message.h"
#include "Locks.hpp"
#include "PrjFSProviderUserClient.hpp"

// Function prototypes
static int HandleVnodeOperation(
    kauth_cred_t    credential,
    void*           idata,
    kauth_action_t  action,
    uintptr_t       arg0,
    uintptr_t       arg1,
    uintptr_t       arg2,
    uintptr_t       arg3);

static int GetPid(vfs_context_t context);

static uint32_t ReadVNodeFileFlags(vnode_t vn, vfs_context_t context);
static bool FileFlagsBitIsSet(uint32_t fileFlags, uint32_t bit);
static bool ActionBitIsSet(kauth_action_t action, kauth_action_t mask);
static bool ActionBitsNotSet(kauth_action_t action, kauth_action_t mask);

static bool IsFileSystemCrawler(char* procname);

static const char* GetRelativePath(const char* path, const char* root);

static void Sleep(int seconds, void* channel);
static bool TrySendRequestAndWaitForResponse(
    const VirtualizationRoot* root,
    MessageType messageType,
    const vnode_t vnode,
    int pid,
    const char* procname,
    int* kauthResult,
    int* kauthError);
static void AbortAllOutstandingEvents();
static bool ShouldIgnoreVnodeType(vtype vnodeType, vnode_t vnode);


// Structs
typedef struct OutstandingMessage
{
    MessageHeader request;
    MessageType response;
    bool    receivedResponse;
    
    LIST_ENTRY(OutstandingMessage) _list_privates;
    
} OutstandingMessage;

// State
static kauth_listener_t s_vnodeListener = nullptr;

static LIST_HEAD(OutstandingMessage_Head, OutstandingMessage) s_outstandingMessages = LIST_HEAD_INITIALIZER(OutstandingMessage_Head);
static Mutex s_outstandingMessagesMutex = {};
static volatile int s_nextMessageId;

static atomic_int s_numActiveKauthEvents;
static volatile bool s_isShuttingDown;

// Public functions
kern_return_t KauthHandler_Init()
{
    if (nullptr != s_vnodeListener)
    {
        goto CleanupAndFail;
    }
    
    LIST_INIT(&s_outstandingMessages);
    s_nextMessageId = 1;
    
    s_isShuttingDown = false;
    
    s_outstandingMessagesMutex = Mutex_Alloc();
    if (!Mutex_IsValid(s_outstandingMessagesMutex))
    {
        goto CleanupAndFail;
    }
        
    if (VirtualizationRoots_Init())
    {
        goto CleanupAndFail;
    }
    
    s_vnodeListener = kauth_listen_scope(KAUTH_SCOPE_VNODE, HandleVnodeOperation, nullptr);
    if (nullptr == s_vnodeListener)
    {
        goto CleanupAndFail;
    }
    
    return KERN_SUCCESS;
    
CleanupAndFail:
    KauthHandler_Cleanup();
    return KERN_FAILURE;
}

kern_return_t KauthHandler_Cleanup()
{
    kern_return_t result = KERN_SUCCESS;
    
    // First, stop new listener callback calls
    if (nullptr != s_vnodeListener)
    {
        kauth_unlisten_scope(s_vnodeListener);
        s_vnodeListener = nullptr;
    }
    else
    {
        result = KERN_FAILURE;
    }

    // Then, ensure there are no more callbacks in flight.
    AbortAllOutstandingEvents();

    if (VirtualizationRoots_Cleanup())
    {
        result = KERN_FAILURE;
    }
        
    if (Mutex_IsValid(s_outstandingMessagesMutex))
    {
        Mutex_FreeMemory(&s_outstandingMessagesMutex);
    }
    else
    {
        result = KERN_FAILURE;
    }
    
    return result;
}

// Private functions
static int HandleVnodeOperation(
    kauth_cred_t    credential,
    void*           idata,
    kauth_action_t  action,
    uintptr_t       arg0,
    uintptr_t       arg1,
    uintptr_t       arg2,
    uintptr_t       arg3)
{
    atomic_fetch_add(&s_numActiveKauthEvents, 1);

    int kauthResult = KAUTH_RESULT_DEFER;
    
    int pid;
    vtype vnodeType;
    char procname[MAXCOMLEN + 1];
    VirtualizationRoot* root = nullptr;
    int16_t rootIndex;
    uint32_t currentVnodeFileFlags;
    
    vfs_context_t context = reinterpret_cast<vfs_context_t>(arg0);
    vnode_t currentVnode =  reinterpret_cast<vnode_t>(arg1);
    // arg2 is the (vnode_t) parent vnode
    int* kauthError =       reinterpret_cast<int*>(arg3);
    
    if (!VirtualizationRoot_VnodeIsOnAllowedFilesystem(currentVnode))
    {
        kauthResult = KAUTH_RESULT_DEFER;
        goto CleanupAndReturn;
    }
    
    vnodeType = vnode_vtype(currentVnode);
    if (ShouldIgnoreVnodeType(vnodeType, currentVnode))
    {
        kauthResult = KAUTH_RESULT_DEFER;
        goto CleanupAndReturn;
    }
    
    pid = GetPid(context);
    
    currentVnodeFileFlags = ReadVNodeFileFlags(currentVnode, context);
    if (!FileFlagsBitIsSet(currentVnodeFileFlags, FileFlags_IsInVirtualizationRoot))
    {
        // This vnode is not part of ANY virtualization root, so exit now before doing any more work.
        // This gives us a cheap way to avoid adding overhead to IO outside of a virtualization root.
        
        kauthResult = KAUTH_RESULT_DEFER;
        goto CleanupAndReturn;
    }
    
    proc_name(pid, procname, sizeof(procname));
    
    if (FileFlagsBitIsSet(currentVnodeFileFlags, FileFlags_IsEmpty))
    {
        // This vnode is not yet hydrated, so do not allow a file system crawler to force hydration.
        // Once a vnode is hydrated, it's fine to allow crawlers to access those contents.
        
        if (IsFileSystemCrawler(procname))
        {
            // We must DENY file system crawlers rather than DEFER.
            // If we allow the crawler's access to succeed without hydrating, the kauth result will be cached and we won't
            // get called again, so we lose the opportunity to hydrate the file/directory and it will appear as though
            // it is missing its contents.

            kauthResult = KAUTH_RESULT_DENY;
            goto CleanupAndReturn;
        }
    }
    
    root = VirtualizationRoots_FindForVnode(currentVnode);
    rootIndex = nullptr == root ? -1 : root->index;
    
    if (nullptr == root)
    {
        KextLog_FileNote(currentVnode, "No virtualization root found for file with set flag.");

        kauthResult = KAUTH_RESULT_DEFER;
        goto CleanupAndReturn;
    }
    else if (nullptr == root->providerUserClient)
    {
        if (rootIndex >= 0)
        {
            bool vnodeIsDir = (vnodeType == VDIR);
            // File/directory is within an offline virtualization root (no provider)
            // Allow read-only access to hydrated files, deny any writes except
            // deletions, and prevent most read accesses to empty files.
            // Empty directories need to be read/searched in order for them to
            // be deleted by rm -r
            if (ActionBitsNotSet(action, KAUTH_VNODE_ACCESS)
                && ActionBitIsSet(
                    action,
                    KAUTH_VNODE_WRITE_ATTRIBUTES |
                    KAUTH_VNODE_WRITE_EXTATTRIBUTES |
                    KAUTH_VNODE_WRITE_DATA |
                    KAUTH_VNODE_APPEND_DATA |
                    KAUTH_VNODE_WRITE_SECURITY |
                    KAUTH_VNODE_LINKTARGET))
            {
                KextLog_FileNote(currentVnode, "HandleVnodeOperation - write action 0x%x by process %u (%s) DENIED  on %s with offline provider.", action, pid, procname, vnodeIsDir ? "directory" : "file");
                kauthResult = KAUTH_RESULT_DENY;
                goto CleanupAndReturn;
            }
            
            if (FileFlagsBitIsSet(currentVnodeFileFlags, FileFlags_IsEmpty))
            {
                // Empty files/directories with offline provider may only be queried or deleted
                if (ActionBitIsSet(action, (KAUTH_VNODE_ACCESS | KAUTH_VNODE_DELETE_CHILD | KAUTH_VNODE_DELETE | KAUTH_VNODE_READ_EXTATTRIBUTES)))
                {
                    kauthResult = KAUTH_RESULT_DEFER;
                    goto CleanupAndReturn;
                }
                
                // Empty directories may additionally have their attributes and security read, and contents listed/searched (otherwise rm -r doesn't work)
                if (vnodeIsDir && ActionBitIsSet(action, (KAUTH_VNODE_READ_ATTRIBUTES | KAUTH_VNODE_READ_SECURITY | KAUTH_VNODE_LIST_DIRECTORY | KAUTH_VNODE_SEARCH)))
                {
                    kauthResult = KAUTH_RESULT_DEFER;
                    goto CleanupAndReturn;
                }
                
                // Disallow any other operations on empty placeholders
                KextLog_FileNote(currentVnode, "HandleVnodeOperation - action 0x%x by process %u (%s) DENIED on empty %s with offline provider.", action, pid, procname, vnodeIsDir ? "directory" : "file");
                kauthResult = KAUTH_RESULT_DENY;
                goto CleanupAndReturn;
            }
        }
        
        kauthResult = KAUTH_RESULT_DEFER;
        goto CleanupAndReturn;
    }
    
    // If the calling process is the provider, we must exit right away to avoid deadlocks
    if (pid == root->providerPid)
    {
        kauthResult = KAUTH_RESULT_DEFER;
        goto CleanupAndReturn;
    }
    
    if (VDIR == vnodeType)
    {
        if (ActionBitIsSet(
                action,
                KAUTH_VNODE_LIST_DIRECTORY |
                KAUTH_VNODE_SEARCH |
                KAUTH_VNODE_READ_SECURITY |
                KAUTH_VNODE_READ_ATTRIBUTES |
                KAUTH_VNODE_READ_EXTATTRIBUTES))
        {
            if (FileFlagsBitIsSet(currentVnodeFileFlags, FileFlags_IsEmpty))
            {
                if (!TrySendRequestAndWaitForResponse(
                        root,
                        MessageType_KtoU_EnumerateDirectory,
                        currentVnode,
                        pid,
                        procname,
                        &kauthResult,
                        kauthError))
                {
                    goto CleanupAndReturn;
                }
            }
        }
    }
    else
    {
        if (ActionBitIsSet(
                action,
                KAUTH_VNODE_READ_ATTRIBUTES |
                KAUTH_VNODE_WRITE_ATTRIBUTES |
                KAUTH_VNODE_READ_EXTATTRIBUTES |
                KAUTH_VNODE_WRITE_EXTATTRIBUTES |
                KAUTH_VNODE_READ_DATA |
                KAUTH_VNODE_WRITE_DATA |
                KAUTH_VNODE_EXECUTE))
        {
            if (FileFlagsBitIsSet(currentVnodeFileFlags, FileFlags_IsEmpty))
            {
                if (!TrySendRequestAndWaitForResponse(
                        root,
                        MessageType_KtoU_HydrateFile,
                        currentVnode,
                        pid,
                        procname,
                        &kauthResult,
                        kauthError))
                {
                    goto CleanupAndReturn;
                }
            }
        }
    }
    
CleanupAndReturn:
    atomic_fetch_sub(&s_numActiveKauthEvents, 1);
    return kauthResult;
}

void KauthHandler_HandleKernelMessageResponse(uint64_t messageId, MessageType responseType)
{
    switch (responseType)
    {
        case MessageType_Response_Success:
        case MessageType_Response_Fail:
        {
            Mutex_Acquire(s_outstandingMessagesMutex);
            {
                OutstandingMessage* outstandingMessage;
                LIST_FOREACH(outstandingMessage, &s_outstandingMessages, _list_privates)
                {
                    if (outstandingMessage->request.messageId == messageId)
                    {
                        // Save the response for the blocked thread.
                        outstandingMessage->response = responseType;
                        outstandingMessage->receivedResponse = true;
                        
                        wakeup(outstandingMessage);
                        
                        break;
                    }
                }
            }
            Mutex_Release(s_outstandingMessagesMutex);
            
        }
    }
    
    return;
}

static bool TrySendRequestAndWaitForResponse(
    const VirtualizationRoot* root,
    MessageType messageType,
    const vnode_t vnode,
    int pid,
    const char* procname,
    int* kauthResult,
    int* kauthError)
{
    bool result = false;
    
    OutstandingMessage message;
    message.receivedResponse = false;
    
    char vnodePath[PrjFSMaxPath];
    int vnodePathLength = PrjFSMaxPath;
    if (vn_getpath(vnode, vnodePath, &vnodePathLength))
    {
        KextLog_Error("Unable to resolve a vnode to its path");
        *kauthResult = KAUTH_RESULT_DENY;
        return false;
    }
    
    const char* relativePath = GetRelativePath(vnodePath, root->path);
    
    int nextMessageId = OSIncrementAtomic(&s_nextMessageId);
    
    Message messageSpec = {};
    Message_Init(&messageSpec, &(message.request), nextMessageId, messageType, pid, procname, relativePath);

    bool isShuttingDown = false;
    Mutex_Acquire(s_outstandingMessagesMutex);
    {
        // Only read s_isShuttingDown once so we either insert & send message, or neither.
        isShuttingDown = s_isShuttingDown;
        if (!isShuttingDown)
        {
            LIST_INSERT_HEAD(&s_outstandingMessages, &message, _list_privates);
        }
    }
    Mutex_Release(s_outstandingMessagesMutex);
    
    if (!isShuttingDown && 0 != ActiveProvider_SendMessage(root->index, messageSpec))
    {
        // TODO: appropriately handle unresponsive providers
        
        *kauthResult = KAUTH_RESULT_DEFER;
        goto CleanupAndReturn;
    }
    
    while (!message.receivedResponse &&
           !s_isShuttingDown)
    {
        Sleep(5, &message);
    }
    
    if (s_isShuttingDown)
    {
        *kauthResult = KAUTH_RESULT_DENY;
        goto CleanupAndReturn;
    }

    if (MessageType_Response_Success == message.response)
    {
        *kauthResult = KAUTH_RESULT_DEFER;
        result = true;
        goto CleanupAndReturn;
    }
    else
    {
        // Default error code is EACCES. See errno.h for more codes.
        *kauthError = EAGAIN;
        *kauthResult = KAUTH_RESULT_DENY;
        goto CleanupAndReturn;
    }
    
CleanupAndReturn:
    Mutex_Acquire(s_outstandingMessagesMutex);
    {
        LIST_REMOVE(&message, _list_privates);
    }
    Mutex_Release(s_outstandingMessagesMutex);
    
    return result;
}

static void AbortAllOutstandingEvents()
{
    // Wake up all sleeping threads so they can see that that we're shutting down and return an error
    Mutex_Acquire(s_outstandingMessagesMutex);
    {
        s_isShuttingDown = true;
        
        OutstandingMessage* outstandingMessage;
        LIST_FOREACH(outstandingMessage, &s_outstandingMessages, _list_privates)
        {
            wakeup(outstandingMessage);
        }
    }
    Mutex_Release(s_outstandingMessagesMutex);
    
    // ... and wait until all kauth events have noticed and returned.
    // Always sleeping at least once reduces the likelihood of a race condition
    // between kauth_unlisten_scope and the s_numActiveKauthEvents increment at
    // the start of the callback.
    // This race condition and the inability to work around it is a longstanding
    // bug in the xnu kernel - see comment block in RemoveListener() function of
    // the KauthORama sample code:
    // https://developer.apple.com/library/archive/samplecode/KauthORama/Listings/KauthORama_c.html#//apple_ref/doc/uid/DTS10003633-KauthORama_c-DontLinkElementID_3
    do
    {
        Sleep(1, NULL);
    } while (atomic_load(&s_numActiveKauthEvents) > 0);
}

static void Sleep(int seconds, void* channel)
{
    struct timespec timeout;
    timeout.tv_sec  = seconds;
    timeout.tv_nsec = 0;
    
    msleep(channel, nullptr, PUSER, "io.gvfs.PrjFSKext.Sleep", &timeout);
}

static int GetPid(vfs_context_t context)
{
    proc_t callingProcess = vfs_context_proc(context);
    return proc_pid(callingProcess);
}

static errno_t GetVNodeAttributes(vnode_t vn, vfs_context_t context, struct vnode_attr* attrs)
{
    VATTR_INIT(attrs);
    VATTR_WANTED(attrs, va_flags);
    
    return vnode_getattr(vn, attrs, context);
}

static uint32_t ReadVNodeFileFlags(vnode_t vn, vfs_context_t context)
{
    struct vnode_attr attributes = {};
    errno_t err = GetVNodeAttributes(vn, context, &attributes);
    // TODO: May fail on some file system types? Perhaps we should early-out depending on mount point anyway.
    assert(0 == err);
    assert(VATTR_IS_SUPPORTED(&attributes, va_flags));
    return attributes.va_flags;
}

static bool FileFlagsBitIsSet(uint32_t fileFlags, uint32_t bit)
{
    // Note: if multiple bits are set in 'bit', this will return true if ANY are set in fileFlags
    return 0 != (fileFlags & bit);
}

static bool ActionBitIsSet(kauth_action_t action, kauth_action_t mask)
{
    return action & mask;
}

static bool ActionBitsNotSet(kauth_action_t action, kauth_action_t mask)
{
    return 0 == (action & mask);
}

static bool IsFileSystemCrawler(char* procname)
{
    // These process will crawl the file system and force a full hydration
    if (!strcmp(procname, "mds") ||
        !strcmp(procname, "mdworker") ||
        !strcmp(procname, "mds_stores") ||
        !strcmp(procname, "fseventsd") ||
        !strcmp(procname, "Spotlight"))
    {
        return true;
    }
    
    return false;
}

static const char* GetRelativePath(const char* path, const char* root)
{
    assert(strlen(path) >= strlen(root));
    
    const char* relativePath = path + strlen(root);
    if (relativePath[0] == '/')
    {
        relativePath++;
    }
    
    return relativePath;
}

static bool ShouldIgnoreVnodeType(vtype vnodeType, vnode_t vnode)
{
    switch (vnodeType)
    {
    case VNON:
    case VBLK:
    case VCHR:
    case VSOCK:
    case VFIFO:
    case VBAD:
        return true;
	case VREG:
    case VDIR:
    case VLNK:
        return false;
    case VSTR:
    case VCPLX:
        {
            char vnodePath[PrjFSMaxPath];
            int vnodePathLength = PrjFSMaxPath;
            vn_getpath(vnode, vnodePath, &vnodePathLength);
            KextLog_Info("vnode with type %s encountered, path %s", vnodeType == VSTR ? "VSTR" : "VCPLX", vnodePath);
            
            return false;
        }
    default:
        KextLog_Info("vnode with unknown type %d encountered", vnodeType);
        return false;
    }
}
