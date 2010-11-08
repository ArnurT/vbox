/* $Id$ */
/** @file
 * VBoxService - Auto-mounting for Shared Folders.
 */

/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <iprt/assert.h>
#include <iprt/dir.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/semaphore.h>
#include <VBox/VBoxGuestLib.h>
#include "VBoxServiceInternal.h"
#include "VBoxServiceUtils.h"

#include <errno.h>
#include <grp.h>
#include <sys/mount.h>
#ifdef RT_OS_SOLARIS
# include <sys/mount.h>
# include <sys/mnttab.h>
# include <sys/vfs.h>
#else
# include <mntent.h>
# include <paths.h>
#endif
#include <unistd.h>

RT_C_DECLS_BEGIN
#include "../../linux/sharedfolders/vbsfmount.h"
RT_C_DECLS_END

#ifdef RT_OS_SOLARIS
# define AUTO_MOUNT_POINT       "/mnt/%s%s"
#else
# define AUTO_MOUNT_POINT       "/media/%s%s"
#endif

#ifndef _PATH_MOUNTED
 #define _PATH_MOUNTED "/etc/mtab"
#endif

/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** The semaphore we're blocking on. */
static RTSEMEVENTMULTI  g_AutoMountEvent = NIL_RTSEMEVENTMULTI;


/** @copydoc VBOXSERVICE::pfnPreInit */
static DECLCALLBACK(int) VBoxServiceAutoMountPreInit(void)
{
    return VINF_SUCCESS;
}


/** @copydoc VBOXSERVICE::pfnOption */
static DECLCALLBACK(int) VBoxServiceAutoMountOption(const char **ppszShort, int argc, char **argv, int *pi)
{
    NOREF(ppszShort);
    NOREF(argc);
    NOREF(argv);
    NOREF(pi);
    return VINF_SUCCESS;
}


/** @copydoc VBOXSERVICE::pfnInit */
static DECLCALLBACK(int) VBoxServiceAutoMountInit(void)
{
    VBoxServiceVerbose(3, "VBoxServiceAutoMountInit\n");

    int rc = RTSemEventMultiCreate(&g_AutoMountEvent);
    AssertRCReturn(rc, rc);

    return rc;
}


static bool VBoxServiceAutoMountShareIsMounted(const char *pszShare,
                                               char *pszMountPoint, size_t cbMountPoint)
{
    AssertPtrReturn(pszShare, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pszMountPoint, VERR_INVALID_PARAMETER);
    AssertReturn(cbMountPoint, VERR_INVALID_PARAMETER);

    bool fMounted = false;
    /* @todo What to do if we have a relative path in mtab instead
     *       of an absolute one ("temp" vs. "/media/temp")?
     * procfs contains the full path but not the actual share name ...
     * FILE *pFh = setmntent("/proc/mounts", "r+t"); */
    FILE *pFh = setmntent(_PATH_MOUNTED, "r+t");
    if (pFh == NULL)
        VBoxServiceError("VBoxServiceAutoMountShareIsMounted: Could not open mtab!\n");
    else
    {
        mntent *pMntEnt;
        while ((pMntEnt = getmntent(pFh)))
        {
            if (!RTStrICmp(pMntEnt->mnt_fsname, pszShare))
            {
                fMounted = RTStrPrintf(pszMountPoint, cbMountPoint, "%s", pMntEnt->mnt_dir)
                         ? true : false;
                break;
            }
        }
        endmntent(pFh);
    }
    return fMounted;
}


static int VBoxServiceAutoMountUnmount(const char *pszMountPoint)
{
    AssertPtrReturn(pszMountPoint, VERR_INVALID_PARAMETER);

    int rc = VINF_SUCCESS;
    uint8_t uTries = 0;
    int r;
    while (uTries++ < 3)
    {
        r = umount(pszMountPoint);
        if (r == 0)
            break;
        RTThreadSleep(5000); /* Wait a while ... */
    }
    if (r == -1)
        rc = RTErrConvertFromErrno(errno);
    return rc;
}


static int VBoxServiceAutoMountPrepareMountPoint(const char *pszMountPoint, const char *pszShareName,
                                                 vbsf_mount_opts *pOpts)
{
    AssertPtrReturn(pOpts, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pszMountPoint, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pszShareName, VERR_INVALID_PARAMETER);

    RTFMODE fMode = RTFS_UNIX_IRWXU | RTFS_UNIX_IRWXG; /* Owner (=root) and the group (=vboxsf) have full access. */
    int rc = RTDirCreateFullPath(pszMountPoint, fMode);
    if (RT_SUCCESS(rc))
    {
        rc = RTPathSetOwnerEx(pszMountPoint, -1 /* Owner, unchanged */, pOpts->gid, RTPATH_F_ON_LINK);
        if (RT_SUCCESS(rc))
        {
            rc = RTPathSetMode(pszMountPoint, fMode);
            if (RT_FAILURE(rc))
                VBoxServiceError(": Could not set mode %RTfmode for mount directory \"%s\", rc = %Rrc\n",
                                 fMode, pszMountPoint, rc);
        }
        else
            VBoxServiceError("VBoxServiceAutoMountPrepareMountPoint: Could not set permissions for mount directory \"%s\", rc = %Rrc\n",
                             pszMountPoint, rc);
    }
    else
        VBoxServiceError("VBoxServiceAutoMountPrepareMountPoint: Could not create mount directory \"%s\" with mode %RTfmode, rc = %Rrc\n",
                         pszMountPoint, fMode, rc);
    return rc;
}


static int VBoxServiceAutoMountSharedFolder(const char *pszShareName, const char *pszMountPoint,
                                            vbsf_mount_opts *pOpts)
{
    AssertPtr(pOpts);

    int rc;
    char szAlreadyMountedTo[RTPATH_MAX];
    /* If a Shared Folder already is mounted but not to our desired mount point,
     * do an unmount first! */
    if (   VBoxServiceAutoMountShareIsMounted(pszShareName, szAlreadyMountedTo, sizeof(szAlreadyMountedTo))
        && RTStrICmp(pszMountPoint, szAlreadyMountedTo))
    {
        VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Shared folder \"%s\" already mounted to \"%s\", unmounting ...\n",
                           pszShareName, szAlreadyMountedTo);
        rc = VBoxServiceAutoMountUnmount(szAlreadyMountedTo);
        if (RT_FAILURE(rc))
            VBoxServiceError("VBoxServiceAutoMountWorker: Failed to unmount \"%s\", %s (%d)!\n",
                             szAlreadyMountedTo, strerror(errno), errno);
    }

    if (RT_SUCCESS(rc))
        rc = VBoxServiceAutoMountPrepareMountPoint(pszMountPoint, pszShareName, pOpts);
    if (RT_SUCCESS(rc))
    {
#ifdef RT_OS_SOLARIS
        int flags = 0; /* No flags used yet. */
        int r = mount(pszShareName,
                      pszMountPoint,
                      flags,
                      "vboxsf",
                      NULL,                     /* char *dataptr */
                      0,                        /* int datalen */
                      NULL,                     /* char *optptr */
                      0);                       /* int optlen */
        if (r == 0)
        {
            VBoxServiceVerbose(0, "VBoxServiceAutoMountWorker: Shared folder \"%s\" was mounted to \"%s\"\n", pszShareName, pszMountPoint);
        }
        else
        {
            if (errno != EBUSY) /* Share is already mounted? Then skip error msg. */
                VBoxServiceError("VBoxServiceAutoMountWorker: Could not mount shared folder \"%s\" to \"%s\", error = %s\n",
                                 pszShareName, pszMountPoint, strerror(errno));
        }
#else /* !RT_OS_SOLARIS */
        unsigned long flags = MS_NODEV;

        const char *szOptions = { "rw" };
        struct vbsf_mount_info_new mntinf;

        mntinf.nullchar     = '\0';
        mntinf.signature[0] = VBSF_MOUNT_SIGNATURE_BYTE_0;
        mntinf.signature[1] = VBSF_MOUNT_SIGNATURE_BYTE_1;
        mntinf.signature[2] = VBSF_MOUNT_SIGNATURE_BYTE_2;
        mntinf.length       = sizeof(mntinf);

        mntinf.uid   = pOpts->uid;
        mntinf.gid   = pOpts->gid;
        mntinf.ttl   = pOpts->ttl;
        mntinf.dmode = pOpts->dmode;
        mntinf.fmode = pOpts->fmode;
        mntinf.dmask = pOpts->dmask;
        mntinf.fmask = pOpts->fmask;

        strcpy(mntinf.name, pszShareName);
        strcpy(mntinf.nls_name, "\0");

        int r = mount(NULL,
                      pszMountPoint,
                      "vboxsf",
                      flags,
                      &mntinf);
        if (r == 0)
        {
            VBoxServiceVerbose(0, "VBoxServiceAutoMountWorker: Shared folder \"%s\" was mounted to \"%s\"\n", pszShareName, pszMountPoint);

            r = vbsfmount_complete(pszShareName, pszMountPoint, flags, pOpts);
            switch (r)
            {
                case 0: /* Success. */
                    errno = 0; /* Clear all errors/warnings. */
                    break;

                case 1:
                    VBoxServiceError("VBoxServiceAutoMountWorker: Could not update mount table (failed to create memstream): %s\n", strerror(errno));
                    break;

                case 2:
                    VBoxServiceError("VBoxServiceAutoMountWorker: Could not open mount table for update: %s\n", strerror(errno));
                    break;

                case 3:
                    VBoxServiceError("VBoxServiceAutoMountWorker: Could not add an entry to the mount table: %s\n", strerror(errno));
                    break;

                default:
                    VBoxServiceError("VBoxServiceAutoMountWorker: Unknown error while completing mount operation: %d\n", r);
                    break;
            }
        }
        else /* r == -1, we got some error in errno.  */
        {
            if (errno == EPROTO)
            {
                VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Messed up share name, re-trying ...\n");

                /* Sometimes the mount utility messes up the share name.  Try to
                 * un-mangle it again. */
                char szCWD[4096];
                size_t cchCWD;
                if (!getcwd(szCWD, sizeof(szCWD)))
                    VBoxServiceError("VBoxServiceAutoMountWorker: Failed to get the current working directory\n");
                cchCWD = strlen(szCWD);
                if (!strncmp(pszMountPoint, szCWD, cchCWD))
                {
                    while (pszMountPoint[cchCWD] == '/')
                        ++cchCWD;
                    /* We checked before that we have enough space */
                    strcpy(mntinf.name, pszMountPoint + cchCWD);
                }
                r = mount(NULL, pszMountPoint, "vboxsf", flags, &mntinf);
            }
            if (errno == EPROTO)
            {
                VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Re-trying with old mounting structure ...\n");

                /* New mount tool with old vboxsf module? Try again using the old
                 * vbsf_mount_info_old structure. */
                struct vbsf_mount_info_old mntinf_old;
                memcpy(&mntinf_old.name, &mntinf.name, MAX_HOST_NAME);
                memcpy(&mntinf_old.nls_name, mntinf.nls_name, MAX_NLS_NAME);
                mntinf_old.uid = mntinf.uid;
                mntinf_old.gid = mntinf.gid;
                mntinf_old.ttl = mntinf.ttl;
                r = mount(NULL, pszMountPoint, "vboxsf", flags, &mntinf_old);
            }
            if (r == -1) /* Was there some error from one of the tries above? */
            {
                switch (errno)
                {
                    /* If we get EINVAL here, the system already has mounted the Shared Folder to another
                     * mount point. */
                    case EINVAL:
                        VBoxServiceVerbose(0, "VBoxServiceAutoMountWorker: Shared folder \"%s\" already is mounted!\n", pszShareName);
                        /* Ignore this error! */
                        break;
                    case EBUSY:
                        /* Ignore these errors! */
                        break;

                    default:
                        VBoxServiceError("VBoxServiceAutoMountWorker: Could not mount shared folder \"%s\" to \"%s\": %s (%d)\n",
                                         pszShareName, pszMountPoint, strerror(errno), errno);
                        rc = RTErrConvertFromErrno(errno);
                        break;
                }
            }
        }
#endif /* !RT_OS_SOLARIS */
    }
    VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Mounting returned with rc=%Rrc\n", rc);
    return rc;
}


/** @copydoc VBOXSERVICE::pfnWorker */
DECLCALLBACK(int) VBoxServiceAutoMountWorker(bool volatile *pfShutdown)
{
    /*
     * Tell the control thread that it can continue
     * spawning services.
     */
    RTThreadUserSignal(RTThreadSelf());

    uint32_t u32ClientId;
    int rc = VbglR3SharedFolderConnect(&u32ClientId);
    if (!RT_SUCCESS(rc))
        VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Failed to connect to the shared folder service, error %Rrc\n", rc);
    else
    {
        uint32_t cMappings;
        VBGLR3SHAREDFOLDERMAPPING *paMappings;

        rc = VbglR3SharedFolderGetMappings(u32ClientId, true /* Only process auto-mounted folders */,
                                           &paMappings, &cMappings);
        if (RT_SUCCESS(rc))
        {
            char *pszSharePrefix;
            rc = VbglR3SharedFolderGetMountPrefix(&pszSharePrefix);
            if (RT_SUCCESS(rc))
            {
                VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Shared folder mount prefix set to \"%s\"\n", pszSharePrefix);
#if 0
                /* Check for a fixed/virtual auto-mount share. */
                if (VbglR3SharedFolderExists(u32ClientId, "vbsfAutoMount"))
                {
                    VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Host supports auto-mount root\n");
                }
                else
                {
#endif
                    VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Got %u shared folder mappings\n", cMappings);
                    for (uint32_t i = 0; i < cMappings && RT_SUCCESS(rc); i++)
                    {
                        char *pszShareName = NULL;
                        rc = VbglR3SharedFolderGetName(u32ClientId, paMappings[i].u32Root, &pszShareName);
                        if (   RT_SUCCESS(rc)
                            && *pszShareName)
                        {
                            VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Connecting share %u (%s) ...\n", i+1, pszShareName);

                            char *pszMountPoint = NULL;
                            if (   RTStrAPrintf(&pszMountPoint, AUTO_MOUNT_POINT, pszSharePrefix, pszShareName) > 0
                                && pszMountPoint)
                            {
                                struct group *grp_vboxsf = getgrnam("vboxsf");
                                if (grp_vboxsf)
                                {
                                    struct vbsf_mount_opts mount_opts =
                                    {
                                        0,                     /* uid */
                                        grp_vboxsf->gr_gid,    /* gid */
                                        0,                     /* ttl */
                                        0770,                  /* dmode, owner and group "vboxsf" have full access */
                                        0770,                  /* fmode, owner and group "vboxsf" have full access */
                                        0,                     /* dmask */
                                        0,                     /* fmask */
                                        0,                     /* ronly */
                                        0,                     /* noexec */
                                        0,                     /* nodev */
                                        0,                     /* nosuid */
                                        0,                     /* remount */
                                        "\0",                  /* nls_name */
                                        NULL,                  /* convertcp */
                                    };

                                    /* We always use "/media" as our root mounting directory. */
                                    /** @todo Detect the correct "media/mnt" directory, based on the current guest (?). */
                                    rc = VBoxServiceAutoMountSharedFolder(pszShareName, pszMountPoint, &mount_opts);
                                }
                                else
                                    VBoxServiceError("VBoxServiceAutoMountWorker: Group \"vboxsf\" does not exist\n");
                                RTStrFree(pszMountPoint);
                            }
                            else
                                rc = VERR_NO_MEMORY;
                            RTStrFree(pszShareName);
                        }
                        else
                            VBoxServiceError("VBoxServiceAutoMountWorker: Error while getting the shared folder name for root node = %u, rc = %Rrc\n",
                                             paMappings[i].u32Root, rc);
                    } /* for cMappings. */
#if 0
                }
#endif
                RTStrFree(pszSharePrefix);
            } /* Mount prefix. */
            else
                VBoxServiceError("VBoxServiceAutoMountWorker: Error while getting the shared folder mount prefix, rc = %Rrc\n", rc);
            RTMemFree(paMappings);
        }
        else
            VBoxServiceError("VBoxServiceAutoMountWorker: Error while getting the shared folder mappings, rc = %Rrc\n", rc);
        VbglR3SharedFolderDisconnect(u32ClientId);
    }

    RTSemEventMultiDestroy(g_AutoMountEvent);
    g_AutoMountEvent = NIL_RTSEMEVENTMULTI;

    VBoxServiceVerbose(3, "VBoxServiceAutoMountWorker: Finished\n");
    return 0;
}

/** @copydoc VBOXSERVICE::pfnTerm */
static DECLCALLBACK(void) VBoxServiceAutoMountTerm(void)
{
    VBoxServiceVerbose(3, "VBoxServiceAutoMountTerm\n");
    return;
}


/** @copydoc VBOXSERVICE::pfnStop */
static DECLCALLBACK(void) VBoxServiceAutoMountStop(void)
{
    RTSemEventMultiSignal(g_AutoMountEvent);
}


/**
 * The 'automount' service description.
 */
VBOXSERVICE g_AutoMount =
{
    /* pszName. */
    "automount",
    /* pszDescription. */
    "Auto-mount for Shared Folders",
    /* pszUsage. */
    NULL,
    /* pszOptions. */
    NULL,
    /* methods */
    VBoxServiceAutoMountPreInit,
    VBoxServiceAutoMountOption,
    VBoxServiceAutoMountInit,
    VBoxServiceAutoMountWorker,
    VBoxServiceAutoMountStop,
    VBoxServiceAutoMountTerm
};
