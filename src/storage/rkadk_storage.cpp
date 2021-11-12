/*
 * Copyright (c) 2021 Rockchip, Inc. All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include "rkadk_storage.h"

#define MAX_TYPE_NMSG_LEN 32
#define MAX_ATTR_LEN 256
#define MAX_STRLINE_LEN 1024

typedef RKADK_S32 (*RKADK_REC_MSG_CB)(RKADK_MW_PTR, RKADK_S32, RKADK_MW_PTR,
                                      RKADK_S32, RKADK_MW_PTR);

typedef struct _RKADK_STR_FILE {
  _RKADK_STR_FILE *next;
  RKADK_CHAR filename[RKADK_MAX_FILE_PATH_LEN];
  time_t stTime;
  off_t stSize;
  off_t stSpace;
  mode_t stMode;
} RKADK_STR_FILE;

typedef struct {
  RKADK_CHAR cpath[RKADK_MAX_FILE_PATH_LEN];
  RKADK_SORT_CONDITION s32SortCond;
  RKADK_S32 wd;
  RKADK_S32 s32FileNum;
  off_t totalSize;
  off_t totalSpace;
  pthread_mutex_t mutex;
  RKADK_STR_FILE *pstFileListFirst;
  RKADK_STR_FILE *pstFileListLast;
} RKADK_STR_FOLDER;

typedef struct {
  RKADK_CHAR cDevPath[RKADK_MAX_FILE_PATH_LEN];
  RKADK_CHAR cDevType[MAX_TYPE_NMSG_LEN];
  RKADK_CHAR cDevAttr1[MAX_ATTR_LEN];
  RKADK_MOUNT_STATUS s32MountStatus;
  pthread_t fileScanTid;
  RKADK_S32 s32FolderNum;
  RKADK_S32 s32TotalSize;
  RKADK_S32 s32FreeSize;
  RKADK_STR_FOLDER *pstFolder;
} RKADK_STR_DEV_STA;

typedef struct _RKADK_TMSG_ELEMENT {
  _RKADK_TMSG_ELEMENT *next;
  RKADK_S32 msg;
  RKADK_CHAR *data;
  RKADK_S32 s32DataLen;
} RKADK_TMSG_ELEMENT;

typedef struct {
  RKADK_TMSG_ELEMENT *first;
  RKADK_TMSG_ELEMENT *last;
  RKADK_S32 num;
  RKADK_S32 quit;
  pthread_mutex_t mutex;
  pthread_cond_t notEmpty;
  RKADK_REC_MSG_CB recMsgCb;
  pthread_t recTid;
  RKADK_MW_PTR pHandlePath;
} RKADK_TMSG_BUFFER;

typedef enum {
  MSG_DEV_ADD = 1,
  MSG_DEV_REMOVE = 2,
  MSG_DEV_CHANGED = 3,
} RKADK_ENUM_MSG;

typedef struct {
  RKADK_TMSG_BUFFER stMsgHd;
  pthread_t eventListenerTid;
  RKADK_S32 eventListenerRun;
  RKADK_STR_DEV_STA stDevSta;
  RKADK_STR_DEV_ATTR stDevAttr;
} RKADK_STORAGE_HANDLE;

static RKADK_STR_DEV_ATTR
RKADK_STORAGE_GetParam(RKADK_STORAGE_HANDLE *pHandle) {
  return pHandle->stDevAttr;
}

RKADK_STR_DEV_ATTR RKADK_STORAGE_GetDevAttr(RKADK_MW_PTR pHandle) {
  return RKADK_STORAGE_GetParam((RKADK_STORAGE_HANDLE *)pHandle);
}

static RKADK_S32 RKADK_STORAGE_CreateFolder(RKADK_CHAR *folder) {
  RKADK_S32 i, len;

  RKADK_CHECK_POINTER(folder, RKADK_FAILURE);

  len = strlen(folder);
  if (!len) {
    RKADK_LOGE("Invalid path.");
    return -1;
  }

  for (i = 1; i < len; i++) {
    if (folder[i] != '/')
      continue;

    folder[i] = 0;
    if (access(folder, R_OK)) {
      if (mkdir(folder, 0755)) {
        RKADK_LOGE("mkdir error");
        return -1;
      }
    }
    folder[i] = '/';
  }

  if (access(folder, R_OK)) {
    if (mkdir(folder, 0755)) {
      RKADK_LOGE("mkdir error");
      return -1;
    }
  }

  RKADK_LOGD("Create %s finished", folder);
  return 0;
}

static RKADK_S32 RKADK_STORAGE_ReadTimeout(RKADK_S32 fd, RKADK_U32 u32WaitMs) {
  RKADK_S32 ret = 0;

  if (u32WaitMs > 0) {
    fd_set readFdset;
    struct timeval timeout;

    FD_ZERO(&readFdset);
    FD_SET(fd, &readFdset);

    timeout.tv_sec = u32WaitMs / 1000;
    timeout.tv_usec = (u32WaitMs % 1000) * 1000;

    do {
      ret = select(fd + 1, &readFdset, NULL, NULL, &timeout);
    } while (ret < 0 && errno == EINTR);

    if (ret == 0) {
      ret = -1;
      errno = ETIMEDOUT;
    } else if (ret == 1) {
      return 0;
    }
  }

  return ret;
}

static RKADK_S32 RKADK_STORAGE_GetDiskSize(RKADK_CHAR *path,
                                           RKADK_S32 *totalSize,
                                           RKADK_S32 *freeSize) {
  struct statfs diskInfo;

  RKADK_CHECK_POINTER(path, RKADK_FAILURE);
  RKADK_CHECK_POINTER(totalSize, RKADK_FAILURE);
  RKADK_CHECK_POINTER(freeSize, RKADK_FAILURE);

  if (statfs(path, &diskInfo)) {
    RKADK_LOGE("statfs[%s] failed", path);
    return -1;
  }

  *totalSize = (diskInfo.f_bsize * diskInfo.f_blocks) >> 10;
  *freeSize = (diskInfo.f_bfree * diskInfo.f_bsize) >> 10;
  return 0;
}

static RKADK_S32 RKADK_STORAGE_GetMountDev(RKADK_CHAR *path, RKADK_CHAR *dev,
                                           RKADK_CHAR *type,
                                           RKADK_CHAR *attributes) {
  FILE *fp;
  RKADK_CHAR strLine[MAX_STRLINE_LEN];
  RKADK_CHAR *tmp;

  RKADK_CHECK_POINTER(dev, RKADK_FAILURE);
  RKADK_CHECK_POINTER(path, RKADK_FAILURE);
  RKADK_CHECK_POINTER(type, RKADK_FAILURE);
  RKADK_CHECK_POINTER(attributes, RKADK_FAILURE);

  if ((fp = fopen("/proc/mounts", "r")) == NULL) {
    RKADK_LOGE("Open file error!");
    return -1;
  }

  while (!feof(fp)) {
    fgets(strLine, MAX_STRLINE_LEN, fp);
    tmp = strstr(strLine, path);

    if (tmp) {
      RKADK_CHAR MountPath[RKADK_MAX_FILE_PATH_LEN];
      sscanf(strLine, "%s %s %s %s", dev, MountPath, type, attributes);

      fclose(fp);
      return 0;
    }
  }

  fclose(fp);
  return -1;
}

static RKADK_S32 RKADK_STORAGE_GetMountPath(RKADK_CHAR *dev, RKADK_CHAR *path,
                                            RKADK_S32 s32PathLen) {
  RKADK_S32 ret = -1;
  FILE *fp;
  RKADK_CHAR strLine[MAX_STRLINE_LEN];
  RKADK_CHAR *tmp;

  RKADK_CHECK_POINTER(dev, RKADK_FAILURE);
  RKADK_CHECK_POINTER(path, RKADK_FAILURE);

  if ((fp = fopen("/proc/mounts", "r")) == NULL) {
    RKADK_LOGE("Open file error!");
    return -1;
  }

  memset(path, 0, s32PathLen);
  while (!feof(fp)) {
    fgets(strLine, MAX_STRLINE_LEN, fp);
    tmp = strstr(strLine, dev);

    if (tmp) {
      RKADK_S32 len;
      RKADK_CHAR *s = strstr(strLine, " ") + 1;
      RKADK_CHAR *e = strstr(s, " ");
      len = e - s;

      if ((len > 0) && (len < s32PathLen)) {
        memcpy(path, s, len);
        ret = 0;
      } else {
        RKADK_LOGE("len[%d], s32PathLen[%d]", len, s32PathLen);
        ret = -2;
      }

      goto exit;
    }
  }

exit:
  fclose(fp);
  return ret;
}

static bool RKADK_STORAGE_FileCompare(RKADK_STR_FILE *existingFile,
                                      RKADK_STR_FILE *newFile,
                                      RKADK_SORT_CONDITION cond) {
  bool ret = false;

  switch (cond) {
  case SORT_MODIFY_TIME: {
    ret = (newFile->stTime <= existingFile->stTime);
    break;
  }
  case SORT_FILE_NAME: {
    ret = (strcmp(newFile->filename, existingFile->filename) <= 0);
    break;
  }
  case SORT_BUTT: {
    ret = false;
    RKADK_LOGE("Invalid condition.");
    break;
  }
  }

  return ret;
}

static RKADK_S32 RKADK_STORAGE_FileListAdd(RKADK_STR_FOLDER *folder,
                                           RKADK_CHAR *filename,
                                           struct stat *statbuf) {
  RKADK_STR_FILE *tmp = NULL;

  RKADK_CHECK_POINTER(folder, RKADK_FAILURE);
  RKADK_CHECK_POINTER(filename, RKADK_FAILURE);

  pthread_mutex_lock(&folder->mutex);

  if (folder->pstFileListFirst) {
    RKADK_STR_FILE *tmp_1 = NULL;
    tmp = folder->pstFileListFirst;

    if (!strcmp(tmp->filename, filename)) {
      folder->pstFileListFirst = tmp->next;
      tmp_1 = tmp;
      tmp_1->stSize = statbuf->st_size;
      tmp_1->stSpace = statbuf->st_blocks * 512;
      tmp_1->stTime = statbuf->st_mtime;
      tmp_1->next = NULL;
    } else {
      while (tmp->next) {
        if (!strcmp(tmp->next->filename, filename)) {
          tmp_1 = tmp->next;
          tmp->next = tmp->next->next;
          tmp_1->stSize = statbuf->st_size;
          tmp_1->stSpace = statbuf->st_blocks * 512;
          tmp_1->stTime = statbuf->st_mtime;
          tmp_1->next = NULL;
          break;
        }
        tmp = tmp->next;
      }
    }

    if (tmp_1 == NULL) {
      tmp_1 = (RKADK_STR_FILE *)malloc(sizeof(RKADK_STR_FILE));
      if (!tmp_1) {
        RKADK_LOGE("tmp_1 malloc failed.");
        pthread_mutex_unlock(&folder->mutex);
        return -1;
      }

      memset(tmp_1, 0, sizeof(RKADK_STR_FILE));
      sprintf(tmp_1->filename, "%s", filename);
      tmp_1->stSize = statbuf->st_size;
      tmp_1->stSpace = statbuf->st_blocks * 512;
      tmp_1->stTime = statbuf->st_mtime;
      tmp_1->next = NULL;
    }

    if (folder->pstFileListFirst) {
      tmp = folder->pstFileListFirst;
      if (RKADK_STORAGE_FileCompare(tmp, tmp_1, folder->s32SortCond)) {
        tmp_1->next = tmp;
        folder->pstFileListFirst = tmp_1;
      } else {
        while (tmp->next) {
          if (RKADK_STORAGE_FileCompare(tmp->next, tmp_1,
                                        folder->s32SortCond)) {
            tmp_1->next = tmp->next;
            tmp->next = tmp_1;
            break;
          }
          tmp = tmp->next;
        }
        if (tmp->next == NULL) {
          tmp->next = tmp_1;
        }
      }
    } else {
      folder->pstFileListFirst = tmp_1;
    }
  } else {
    tmp = (RKADK_STR_FILE *)malloc(sizeof(RKADK_STR_FILE));

    if (!tmp) {
      RKADK_LOGE("tmp malloc failed.");
      pthread_mutex_unlock(&folder->mutex);
      return -1;
    }

    memset(tmp, 0, sizeof(RKADK_STR_FILE));
    sprintf(tmp->filename, "%s", filename);
    folder->pstFileListFirst = tmp;
    tmp->stSize = statbuf->st_size;
    tmp->stSpace = statbuf->st_blocks * 512;
    tmp->stTime = statbuf->st_mtime;
  }
  folder->s32FileNum = 0;
  folder->totalSize = 0;
  folder->totalSpace = 0;
  tmp = folder->pstFileListFirst;
  while (tmp) {
    folder->s32FileNum++;
    folder->totalSize += tmp->stSize;
    folder->totalSpace += tmp->stSpace;
    tmp = tmp->next;
  }

  pthread_mutex_unlock(&folder->mutex);
  return 0;
}

static RKADK_S32 RKADK_STORAGE_FileListDel(RKADK_STR_FOLDER *folder,
                                           RKADK_CHAR *filename) {
  RKADK_S32 s32FileNum = 0;
  off_t totalSize = 0;
  off_t totalSpace = 0;
  RKADK_STR_FILE *next = NULL;

  RKADK_CHECK_POINTER(folder, RKADK_FAILURE);
  RKADK_CHECK_POINTER(filename, RKADK_FAILURE);

  pthread_mutex_lock(&folder->mutex);

  if (folder->pstFileListFirst) {
    RKADK_STR_FILE *tmp = folder->pstFileListFirst;
    if (!strcmp(tmp->filename, filename)) {
      folder->pstFileListFirst = folder->pstFileListFirst->next;
      free(tmp);
      tmp = folder->pstFileListFirst;
    }

    while (tmp) {
      next = tmp->next;
      totalSize += tmp->stSize;
      totalSpace += tmp->stSpace;
      s32FileNum++;
      if (next == NULL)
        break;
      if (!strcmp(next->filename, filename)) {
        tmp->next = next->next;
        free(next);
        next = tmp->next;
      }
      tmp = next;
    }
    folder->pstFileListLast = tmp;
  }
  folder->s32FileNum = s32FileNum;
  folder->totalSize = totalSize;
  folder->totalSpace = totalSpace;

  pthread_mutex_unlock(&folder->mutex);
  return 0;
}

static RKADK_MW_PTR RKADK_STORAGE_FileMonitorThread(RKADK_MW_PTR arg) {
  RKADK_STORAGE_HANDLE *pHandle = (RKADK_STORAGE_HANDLE *)arg;
  RKADK_S32 fd;
  RKADK_S32 len;
  RKADK_S32 nread;
  RKADK_CHAR buf[BUFSIZ];
  struct inotify_event *event;
  RKADK_S32 j;

  if (!pHandle) {
    RKADK_LOGE("invalid pHandle");
    return NULL;
  }

  prctl(PR_SET_NAME, "RKADK_STORAGE_FileMonitorThread", 0, 0, 0);
  fd = inotify_init();
  if (fd < 0) {
    RKADK_LOGE("inotify_init failed");
    return NULL;
  }

  for (j = 0; j < pHandle->stDevSta.s32FolderNum; j++) {
    pHandle->stDevSta.pstFolder[j].wd =
        inotify_add_watch(fd, pHandle->stDevSta.pstFolder[j].cpath,
                          IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM |
                              IN_CLOSE_WRITE | IN_UNMOUNT);
  }

  memset(buf, 0, BUFSIZ);
  while (pHandle->stDevSta.s32MountStatus == DISK_MOUNTED) {
    if (RKADK_STORAGE_ReadTimeout(fd, 10))
      continue;

    len = read(fd, buf, BUFSIZ - 1);
    nread = 0;
    while (len > 0) {
      event = (struct inotify_event *)&buf[nread];
      if (event->mask & IN_UNMOUNT)
        pHandle->stDevSta.s32MountStatus = DISK_UNMOUNTED;

      if (event->len > 0) {
        for (j = 0; j < pHandle->stDevSta.s32FolderNum; j++) {
          if (event->wd == pHandle->stDevSta.pstFolder[j].wd) {
            if (event->mask & IN_MOVED_TO) {
              RKADK_CHAR d_name[RKADK_MAX_FILE_PATH_LEN];
              struct stat statbuf;
              sprintf(d_name, "%s%s", pHandle->stDevSta.pstFolder[j].cpath,
                      event->name);
              if (lstat(d_name, &statbuf)) {
                RKADK_LOGE("lstat[%s](IN_MOVED_TO) failed", d_name);
              } else {
                if (RKADK_STORAGE_FileListAdd(&pHandle->stDevSta.pstFolder[j],
                                              event->name, &statbuf))
                  RKADK_LOGE("FileListAdd failed");
              }
            }

            if ((event->mask & IN_DELETE) || (event->mask & IN_MOVED_FROM))
              if (RKADK_STORAGE_FileListDel(&pHandle->stDevSta.pstFolder[j],
                                            event->name))
                RKADK_LOGE("FileListDel failed");

            if (event->mask & IN_CLOSE_WRITE) {
              RKADK_CHAR d_name[RKADK_MAX_FILE_PATH_LEN];
              struct stat statbuf;
              sprintf(d_name, "%s%s", pHandle->stDevSta.pstFolder[j].cpath,
                      event->name);
              if (lstat(d_name, &statbuf)) {
                RKADK_LOGE("lstat[%s](IN_CLOSE_WRITE) failed", d_name);
              } else {
                if (RKADK_STORAGE_FileListAdd(&pHandle->stDevSta.pstFolder[j],
                                              event->name, &statbuf))
                  RKADK_LOGE("FileListAdd failed");
              }
            }
          }
        }
      }

      nread = nread + sizeof(struct inotify_event) + event->len;
      len = len - sizeof(struct inotify_event) - event->len;
    }
  }

  RKADK_LOGD("Exit!");
  close(fd);
  return NULL;
}

static RKADK_MW_PTR RKADK_STORAGE_FileScanThread(RKADK_MW_PTR arg) {
  RKADK_STORAGE_HANDLE *pHandle = (RKADK_STORAGE_HANDLE *)arg;
  RKADK_S32 cnt = 0;
  RKADK_S32 i;
  pthread_t fileMonitorTid = 0;
  RKADK_STR_DEV_ATTR devAttr;

  if (!pHandle) {
    RKADK_LOGE("invalid pHandle");
    return NULL;
  }

  devAttr = RKADK_STORAGE_GetParam(pHandle);
  prctl(PR_SET_NAME, "file_scan_thread", 0, 0, 0);
  RKADK_LOGI("%s, %s, %s, %s", devAttr.cMountPath, pHandle->stDevSta.cDevPath,
             pHandle->stDevSta.cDevType, pHandle->stDevSta.cDevAttr1);

  if (pHandle->stDevSta.s32MountStatus == DISK_MOUNTED) {
    if (RKADK_STORAGE_GetDiskSize(devAttr.cMountPath,
                                  &pHandle->stDevSta.s32TotalSize,
                                  &pHandle->stDevSta.s32FreeSize)) {
      RKADK_LOGE("GetDiskSize failed");
      return NULL;
    }
  } else {
    pHandle->stDevSta.s32TotalSize = 0;
    pHandle->stDevSta.s32FreeSize = 0;
  }
  RKADK_LOGI("s32TotalSize = %d, s32FreeSize = %d",
             pHandle->stDevSta.s32TotalSize, pHandle->stDevSta.s32FreeSize);

  if (pHandle->stDevSta.s32MountStatus == DISK_MOUNTED) {
    RKADK_LOGI("devAttr.s32FolderNum = %d", devAttr.s32FolderNum);
    pHandle->stDevSta.s32FolderNum = devAttr.s32FolderNum;
    pHandle->stDevSta.pstFolder = (RKADK_STR_FOLDER *)malloc(
        sizeof(RKADK_STR_FOLDER) * devAttr.s32FolderNum);

    if (!pHandle->stDevSta.pstFolder) {
      RKADK_LOGE("pHandle->stDevSta.pstFolder malloc failed.");
      return NULL;
    }
    memset(pHandle->stDevSta.pstFolder, 0,
           sizeof(RKADK_STR_FOLDER) * devAttr.s32FolderNum);
    for (i = 0; i < pHandle->stDevSta.s32FolderNum; i++) {
      sprintf(pHandle->stDevSta.pstFolder[i].cpath, "%s%s", devAttr.cMountPath,
              devAttr.pstFolderAttr[i].cFolderPath);
      pHandle->stDevSta.pstFolder[i].s32SortCond =
          devAttr.pstFolderAttr[i].s32SortCond;
      RKADK_LOGI("%s", pHandle->stDevSta.pstFolder[i].cpath);
      pthread_mutex_init(&(pHandle->stDevSta.pstFolder[i].mutex), NULL);
      if (RKADK_STORAGE_CreateFolder(pHandle->stDevSta.pstFolder[i].cpath)) {
        RKADK_LOGE("CreateFolder failed");
        goto file_scan_out;
      }
    }
  }

  if (pthread_create(&fileMonitorTid, NULL, RKADK_STORAGE_FileMonitorThread,
                     (RKADK_MW_PTR)pHandle)) {
    RKADK_LOGE("FileMonitorThread create failed.");
    goto file_scan_out;
  }

  if (pHandle->stDevSta.s32MountStatus == DISK_MOUNTED) {
    for (i = 0; i < pHandle->stDevSta.s32FolderNum; i++) {
      DIR *dp;
      struct dirent *entry;
      struct stat statbuf;

      if ((dp = opendir(pHandle->stDevSta.pstFolder[i].cpath)) == NULL) {
        RKADK_LOGE("Open %s error", pHandle->stDevSta.pstFolder[i].cpath);
        continue;
      }

      chdir(pHandle->stDevSta.pstFolder[i].cpath);
      while (((entry = readdir(dp)) != NULL) &&
             (pHandle->stDevSta.s32MountStatus == DISK_MOUNTED) &&
             devAttr.s32AutoDel) {
        if (lstat(entry->d_name, &statbuf)) {
          RKADK_LOGE("lstat[%s] failed", entry->d_name);
          break;
        }

        if (!S_ISDIR(statbuf.st_mode)) {
          if (RKADK_STORAGE_FileListAdd(&pHandle->stDevSta.pstFolder[i],
                                        entry->d_name, &statbuf)) {
            RKADK_LOGE("FileListAdd failed");
            break;
          }
        }

        if (pHandle->stDevSta.pstFolder[i].s32FileNum % 100 == 0)
          usleep(100);
      }
      chdir("/");
      closedir(dp);
      RKADK_LOGI("s32FileNum = %d, totalSize = %lld, TotalSpace = %lld",
                 pHandle->stDevSta.pstFolder[i].s32FileNum,
                 pHandle->stDevSta.pstFolder[i].totalSize,
                 pHandle->stDevSta.pstFolder[i].totalSpace);
    }
  }

  while (pHandle->stDevSta.s32MountStatus == DISK_MOUNTED) {
    if (cnt++ > 50) {
      RKADK_S32 limit;
      off_t totalSpace = 0;
      cnt = 0;

      if (RKADK_STORAGE_GetDiskSize(devAttr.cMountPath,
                                    &pHandle->stDevSta.s32TotalSize,
                                    &pHandle->stDevSta.s32FreeSize)) {
        RKADK_LOGE("GetDiskSize failed");
        goto file_scan_out;
      }

      if (pHandle->stDevSta.s32FreeSize <= (devAttr.s32FreeSizeDelMin * 1024))
        devAttr.s32AutoDel = 1;

      if (pHandle->stDevSta.s32FreeSize >= (devAttr.s32FreeSizeDelMax * 1024))
        devAttr.s32AutoDel = 0;

      if (devAttr.s32AutoDel) {
        for (i = 0; i < devAttr.s32FolderNum; i++) {
          pthread_mutex_lock(&pHandle->stDevSta.pstFolder[i].mutex);
          totalSpace += pHandle->stDevSta.pstFolder[i].totalSpace;
          pthread_mutex_unlock(&pHandle->stDevSta.pstFolder[i].mutex);
        }
        for (i = 0; i < devAttr.s32FolderNum && totalSpace; i++) {
          RKADK_CHAR file[3 * RKADK_MAX_FILE_PATH_LEN];
          pthread_mutex_lock(&pHandle->stDevSta.pstFolder[i].mutex);
          if (devAttr.pstFolderAttr[i].bNumLimit == RKADK_FALSE)
            limit =
                pHandle->stDevSta.pstFolder[i].totalSpace * 100 / totalSpace;
          else
            limit = pHandle->stDevSta.pstFolder[i].s32FileNum;

          if (limit > devAttr.pstFolderAttr[i].s32Limit) {
            if (pHandle->stDevSta.pstFolder[i].pstFileListFirst) {
              sprintf(
                  file, "%s%s%s", devAttr.cMountPath,
                  devAttr.pstFolderAttr[i].cFolderPath,
                  pHandle->stDevSta.pstFolder[i].pstFileListFirst->filename);
              RKADK_LOGI("Delete file:%s", file);
              pthread_mutex_unlock(&pHandle->stDevSta.pstFolder[i].mutex);

              if (remove(file))
                RKADK_LOGE("Delete %s file error.", file);
              usleep(100);
              cnt = 51;
              continue;
            }
          }
          pthread_mutex_unlock(&pHandle->stDevSta.pstFolder[i].mutex);
          RKADK_LOGI("%s %d", pHandle->stDevSta.pstFolder[i].cpath, limit);
        }
      }
    }
    usleep(10000);
  }

file_scan_out:
  if (fileMonitorTid)
    if (pthread_join(fileMonitorTid, NULL))
      RKADK_LOGE("FileMonitorThread join failed.");
  RKADK_LOGD("out");

  if (pHandle->stDevSta.pstFolder) {
    free(pHandle->stDevSta.pstFolder);
    pHandle->stDevSta.pstFolder = NULL;
  }
  pHandle->stDevSta.s32FolderNum = 0;

  return NULL;
}

static RKADK_S32 RKADK_STORAGE_FSCK(RKADK_CHAR *dev)
{
  if (fork() == 0) {
    RKADK_LOGE("fsck.fat %s\n", dev);
    if (execl("/sbin/fsck.fat", "fsck.fat", "-a", dev, NULL) < 0) {
      RKADK_LOGE("fsck.fat failed:%s\n", strerror(errno));
      return -1;
    }
    usleep(100000);
  }

  return 0;
}

static RKADK_S32 RKADK_STORAGE_DevAdd(RKADK_CHAR *dev,
                                      RKADK_STORAGE_HANDLE *pHandle) {
  RKADK_S32 ret;
  RKADK_STR_DEV_ATTR stDevAttr;
  RKADK_CHAR mountPath[RKADK_MAX_FILE_PATH_LEN];

  RKADK_CHECK_POINTER(dev, RKADK_FAILURE);
  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);

  ret = RKADK_STORAGE_GetMountPath(dev, mountPath, RKADK_MAX_FILE_PATH_LEN);
  if (ret) {
    RKADK_LOGE("RKADK_STORAGE_GetMountPath failed[%d]", ret);
    return ret;
  }

  stDevAttr = RKADK_STORAGE_GetParam(pHandle);
  RKADK_LOGI("%s, %s", dev, mountPath);

  if (strcmp(stDevAttr.cMountPath, mountPath)) {
    RKADK_LOGE("stDevAttr.cMountPath[%s] != mountPath[%s]",
               stDevAttr.cMountPath, mountPath);
    return -1;
  }

  ret = RKADK_STORAGE_GetMountDev(
      stDevAttr.cMountPath, pHandle->stDevSta.cDevPath,
      pHandle->stDevSta.cDevType, pHandle->stDevSta.cDevAttr1);
  if (ret) {
    RKADK_LOGE("RKADK_STORAGE_GetMountDev failed[%d]", ret);
    return ret;
  }
  RKADK_STORAGE_FSCK(pHandle->stDevSta.cDevPath);
  pHandle->stDevSta.s32MountStatus = DISK_MOUNTED;
  usleep(10000);
  if (pthread_create(&pHandle->stDevSta.fileScanTid, NULL,
                     RKADK_STORAGE_FileScanThread, (RKADK_MW_PTR)pHandle))
    RKADK_LOGE("FileScanThread create failed.");

  return 0;
}

static RKADK_S32 RKADK_STORAGE_DevRemove(RKADK_CHAR *dev,
                                         RKADK_STORAGE_HANDLE *pHandle) {
  RKADK_CHECK_POINTER(dev, RKADK_FAILURE);
  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);

  if (!strcmp(pHandle->stDevSta.cDevPath, dev)) {
    pHandle->stDevSta.s32MountStatus = DISK_UNMOUNTED;
    pHandle->stDevSta.s32TotalSize = 0;
    pHandle->stDevSta.s32FreeSize = 0;

    if (pHandle->stDevSta.fileScanTid) {
      if (pthread_join(pHandle->stDevSta.fileScanTid, NULL))
        RKADK_LOGE("FileScanThread join failed.");
      pHandle->stDevSta.fileScanTid = 0;
    }
  }

  return 0;
}

static RKADK_S32 RKADK_STORAGE_MsgPutMsgToBuffer(RKADK_TMSG_BUFFER *buf,
                                                 RKADK_TMSG_ELEMENT *elm) {
  RKADK_CHECK_POINTER(buf, RKADK_FAILURE);
  RKADK_CHECK_POINTER(elm, RKADK_FAILURE);

  if (NULL != elm->next)
    elm->next = NULL;

  pthread_mutex_lock(&buf->mutex);
  if (buf->first) {
    RKADK_TMSG_ELEMENT *tmp = buf->first;
    while (tmp->next != NULL) {
      tmp = tmp->next;
    }
    tmp->next = elm;
  } else {
    buf->first = elm;
  }
  buf->num++;

  pthread_cond_signal(&buf->notEmpty);
  pthread_mutex_unlock(&buf->mutex);
  return 0;
}

static RKADK_TMSG_ELEMENT *
RKADK_STORAGE_MsgGetMsgFromBufferTimeout(RKADK_TMSG_BUFFER *buf,
                                         RKADK_S32 s32TimeoutMs) {
  RKADK_TMSG_ELEMENT *elm = NULL;
  struct timeval timeNow;
  struct timespec timeout;

  if (!buf) {
    return NULL;
  }

  pthread_mutex_lock(&buf->mutex);
  if (0 == buf->num) {
    gettimeofday(&timeNow, NULL);
    timeout.tv_sec = timeNow.tv_sec + s32TimeoutMs / 1000;
    timeout.tv_nsec = (timeNow.tv_usec + (s32TimeoutMs % 1000) * 1000) * 1000;
    pthread_cond_timedwait(&buf->notEmpty, &buf->mutex, &timeout);
  }

  if (buf->num > 0) {
    elm = buf->first;
    if (1 == buf->num) {
      buf->first = buf->last = NULL;
      buf->num = 0;
    } else {
      buf->first = buf->first->next;
      buf->num--;
    }
  }

  pthread_mutex_unlock(&buf->mutex);
  return elm;
}

static RKADK_S32 RKADK_STORAGE_MsgFreeMsg(RKADK_TMSG_ELEMENT *elm) {
  RKADK_CHECK_POINTER(elm, RKADK_FAILURE);

  if (elm->data != NULL) {
    free(elm->data);
    elm->data = NULL;
  }
  free(elm);
  elm = NULL;

  return 0;
}

static RKADK_MW_PTR RKADK_STORAGE_MsgRecMsgThread(RKADK_MW_PTR arg) {
  RKADK_TMSG_BUFFER *msgBuffer = (RKADK_TMSG_BUFFER *)arg;

  if (!msgBuffer) {
    RKADK_LOGE("invalid msgBuffer");
    return NULL;
  }

  prctl(PR_SET_NAME, "RKADK_STORAGE_MsgRecMsgThread", 0, 0, 0);
  while (msgBuffer->quit == 0) {
    RKADK_TMSG_ELEMENT *elm =
        RKADK_STORAGE_MsgGetMsgFromBufferTimeout(msgBuffer, 50);

    if (elm) {
      if (msgBuffer->recMsgCb)
        msgBuffer->recMsgCb(msgBuffer, elm->msg, elm->data, elm->s32DataLen,
                            msgBuffer->pHandlePath);
      if (RKADK_STORAGE_MsgFreeMsg(elm))
        RKADK_LOGE("Free msg failed.");
    }
  }

  RKADK_LOGD("out");
  return NULL;
}

static RKADK_S32 RKADK_STORAGE_MsgRecCb(RKADK_MW_PTR hd, RKADK_S32 msg,
                                        RKADK_MW_PTR data, RKADK_S32 s32DataLen,
                                        RKADK_MW_PTR pHandle) {
  RKADK_LOGI("msg = %d", msg);
  switch (msg) {
  case MSG_DEV_ADD:
    if (RKADK_STORAGE_DevAdd((RKADK_CHAR *)data,
                             (RKADK_STORAGE_HANDLE *)pHandle)) {
      RKADK_LOGE("DevAdd failed");
      return -1;
    }
    break;
  case MSG_DEV_REMOVE:
    if (RKADK_STORAGE_DevRemove((RKADK_CHAR *)data,
                                (RKADK_STORAGE_HANDLE *)pHandle)) {
      RKADK_LOGE("DevRemove failed");
      return -1;
    }
    break;
  case MSG_DEV_CHANGED:
    break;
  }

  return 0;
}

static RKADK_S32 RKADK_STORAGE_MsgCreate(RKADK_REC_MSG_CB recMsgCb,
                                         RKADK_STORAGE_HANDLE *pHandle) {
  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);

  pHandle->stMsgHd.first = NULL;
  pHandle->stMsgHd.last = NULL;
  pHandle->stMsgHd.num = 0;
  pHandle->stMsgHd.quit = 0;
  pHandle->stMsgHd.recMsgCb = recMsgCb;
  pHandle->stMsgHd.pHandlePath = (RKADK_MW_PTR)pHandle;

  pthread_mutex_init(&(pHandle->stMsgHd.mutex), NULL);
  pthread_cond_init(&(pHandle->stMsgHd.notEmpty), NULL);
  if (pthread_create(&(pHandle->stMsgHd.recTid), NULL,
                     RKADK_STORAGE_MsgRecMsgThread,
                     (RKADK_MW_PTR)(&pHandle->stMsgHd))) {
    RKADK_LOGE("RecMsgThread create failed!");
    return -1;
  }

  return 0;
}

static RKADK_S32 RKADK_STORAGE_MsgDestroy(RKADK_STORAGE_HANDLE *pHandle) {
  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);

  pHandle->stMsgHd.quit = 1;
  if (pHandle->stMsgHd.recTid)
    if (pthread_join(pHandle->stMsgHd.recTid, NULL)) {
      RKADK_LOGE("RecMsgThread join failed!");
      return -1;
    }

  return 0;
}

static RKADK_S32 RKADK_STORAGE_MsgSendMsg(RKADK_S32 msg, RKADK_CHAR *data,
                                          RKADK_S32 s32DataLen,
                                          RKADK_TMSG_BUFFER *buf) {
  RKADK_TMSG_ELEMENT *elm = NULL;

  RKADK_CHECK_POINTER(buf, RKADK_FAILURE);
  RKADK_CHECK_POINTER(data, RKADK_FAILURE);

  elm = (RKADK_TMSG_ELEMENT *)malloc(sizeof(RKADK_TMSG_ELEMENT));
  if (!elm) {
    RKADK_LOGE("elm malloc failed.");
    return -1;
  }

  memset(elm, 0, sizeof(RKADK_TMSG_ELEMENT));
  elm->msg = msg;
  elm->data = NULL;
  elm->s32DataLen = s32DataLen;

  if (data && s32DataLen > 0) {
    elm->data = (RKADK_CHAR *)malloc(s32DataLen);
    if (!elm->data) {
      RKADK_LOGE("elm->data malloc failed.");
      free(elm);
      return -1;
    }
    memset(elm->data, 0, s32DataLen);
    memcpy(elm->data, data, s32DataLen);
  }

  elm->next = NULL;

  if (RKADK_STORAGE_MsgPutMsgToBuffer(buf, elm)) {
    if (!elm->data)
      free(elm->data);
    free(elm);
    RKADK_LOGE("Put msg to buffer failed.");
    return -1;
  }

  return 0;
}

static RKADK_CHAR *RKADK_STORAGE_Search(RKADK_CHAR *buf, RKADK_S32 len,
                                        const RKADK_CHAR *str) {
  RKADK_CHAR *ret = 0;
  RKADK_S32 i = 0;

  ret = strstr(buf, str);
  if (ret)
    return ret;
  for (i = 1; i < len; i++) {
    if (buf[i - 1] == 0) {
      ret = strstr(&buf[i], str);
      if (ret)
        return ret;
    }
  }
  return ret;
}

static RKADK_CHAR *RKADK_STORAGE_Getparameters(RKADK_CHAR *buf, RKADK_S32 len,
                                               const RKADK_CHAR *str) {
  RKADK_CHAR *ret = RKADK_STORAGE_Search(buf, len, str);

  if (ret)
    ret += strlen(str) + 1;

  return ret;
}

static RKADK_MW_PTR RKADK_STORAGE_EventListenerThread(RKADK_MW_PTR arg) {
  RKADK_STORAGE_HANDLE *pHandle = (RKADK_STORAGE_HANDLE *)arg;
  RKADK_S32 sockfd;
  RKADK_S32 len;
  RKADK_S32 bufLen = 2000;
  RKADK_CHAR buf[bufLen];
  struct iovec iov;
  struct msghdr msg;
  struct sockaddr_nl sa;
  struct timeval timeout;

  if (!pHandle) {
    RKADK_LOGE("invalid pHandle");
    return NULL;
  }

  timeout.tv_sec = 0;
  timeout.tv_usec = 100000;
  prctl(PR_SET_NAME, "event_monitor", 0, 0, 0);

  sa.nl_family = AF_NETLINK;
  sa.nl_groups = NETLINK_KOBJECT_UEVENT;
  sa.nl_pid = 0;
  iov.iov_base = (RKADK_MW_PTR)buf;
  iov.iov_len = bufLen;
  memset(&msg, 0, sizeof(msg));
  msg.msg_name = (RKADK_MW_PTR)&sa;
  msg.msg_namelen = sizeof(sa);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  sockfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
  if (sockfd == -1) {
    RKADK_LOGE("socket creating failed:%s", strerror(errno));
    return NULL;
  }

  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (RKADK_MW_PTR)&timeout,
             (socklen_t)sizeof(struct timeval));

  if (bind(sockfd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
    RKADK_LOGE("bind error:%s", strerror(errno));
    goto err_event_listener;
  }

  while (pHandle->eventListenerRun) {
    len = recvmsg(sockfd, &msg, 0);
    if (len < 0) {
      // RKADK_LOGW("receive time out");
    } else if (len < MAX_TYPE_NMSG_LEN || len > bufLen) {
      RKADK_LOGW("invalid message");
    } else {
      RKADK_CHAR *p = strstr(buf, "libudev");

      if (p == buf) {
        if (RKADK_STORAGE_Search(buf, len, "DEVTYPE=partition") ||
            RKADK_STORAGE_Search(buf, len, "DEVTYPE=disk")) {
          RKADK_CHAR *dev = RKADK_STORAGE_Getparameters(buf, len, "DEVNAME");

          if (RKADK_STORAGE_Search(buf, len, "ACTION=add")) {
            if (RKADK_STORAGE_MsgSendMsg(MSG_DEV_ADD, dev, strlen(dev) + 1,
                                         &(pHandle->stMsgHd)))
              RKADK_LOGE("Send msg: MSG_DEV_ADD failed.");
          } else if (RKADK_STORAGE_Search(buf, len, "ACTION=remove")) {
            RKADK_LOGI("%s remove", dev);
            if (RKADK_STORAGE_MsgSendMsg(MSG_DEV_REMOVE, dev, strlen(dev) + 1,
                                         &(pHandle->stMsgHd)))
              RKADK_LOGE("Send msg: MSG_DEV_REMOVE failed.");
          } else if (RKADK_STORAGE_Search(buf, len, "ACTION=change")) {
            RKADK_LOGI("%s change", dev);
            if (RKADK_STORAGE_MsgSendMsg(MSG_DEV_CHANGED, dev, strlen(dev) + 1,
                                         &(pHandle->stMsgHd)))
              RKADK_LOGE("Send msg: MSG_DEV_CHANGED failed.");
          }
        }
      }
    }
  }
err_event_listener:
  if (close(sockfd))
    RKADK_LOGE("Close sockfd failed.\n");

  RKADK_LOGD("out");
  return NULL;
}

static RKADK_S32 RKADK_STORAGE_ParameterInit(RKADK_STORAGE_HANDLE *pstHandle,
                                             RKADK_STR_DEV_ATTR *pstDevAttr) {
  RKADK_S32 i;

  RKADK_CHECK_POINTER(pstHandle, RKADK_FAILURE);

  if (pstDevAttr) {
    if (pstDevAttr->pstFolderAttr) {
      sprintf(pstHandle->stDevAttr.cMountPath, pstDevAttr->cMountPath);
      pstHandle->stDevAttr.s32AutoDel = pstDevAttr->s32AutoDel;
      pstHandle->stDevAttr.s32FreeSizeDelMin = pstDevAttr->s32FreeSizeDelMin;
      pstHandle->stDevAttr.s32FreeSizeDelMax = pstDevAttr->s32FreeSizeDelMax;
      pstHandle->stDevAttr.s32FolderNum = pstDevAttr->s32FolderNum;

      pstHandle->stDevAttr.pstFolderAttr = (RKADK_STR_FOLDER_ATTR *)malloc(
          sizeof(RKADK_STR_FOLDER_ATTR) * pstHandle->stDevAttr.s32FolderNum);
      if (!pstHandle->stDevAttr.pstFolderAttr) {
        RKADK_LOGE("pstHandle->stDevAttr.pstFolderAttr malloc failed.");
        return -1;
      }
      memset(pstHandle->stDevAttr.pstFolderAttr, 0,
             sizeof(RKADK_STR_FOLDER_ATTR) * pstHandle->stDevAttr.s32FolderNum);

      for (i = 0; i < pstDevAttr->s32FolderNum; i++) {
        pstHandle->stDevAttr.pstFolderAttr[i].s32SortCond =
            pstDevAttr->pstFolderAttr[i].s32SortCond;
        pstHandle->stDevAttr.pstFolderAttr[i].bNumLimit =
            pstDevAttr->pstFolderAttr[i].bNumLimit;
        pstHandle->stDevAttr.pstFolderAttr[i].s32Limit =
            pstDevAttr->pstFolderAttr[i].s32Limit;
        sprintf(pstHandle->stDevAttr.pstFolderAttr[i].cFolderPath,
                pstDevAttr->pstFolderAttr[i].cFolderPath);
      }

      for (i = 0; i < pstDevAttr->s32FolderNum; i++) {
        RKADK_LOGI("DevAttr set:  AutoDel--%d, FreeSizeDel--%d~%d, Path--%s%s, "
                   "Limit--%d",
                   pstHandle->stDevAttr.s32AutoDel,
                   pstHandle->stDevAttr.s32FreeSizeDelMin,
                   pstHandle->stDevAttr.s32FreeSizeDelMax,
                   pstHandle->stDevAttr.cMountPath,
                   pstHandle->stDevAttr.pstFolderAttr[i].cFolderPath,
                   pstHandle->stDevAttr.pstFolderAttr[i].s32Limit);
      }

      RKADK_LOGD("Set user-defined device attributes done.");
      return 0;
    } else {
      RKADK_LOGE("The device attributes set failed.");
      return -1;
    }
  }

  RKADK_LOGD("Set default device attributes.");
  sprintf(pstHandle->stDevAttr.cMountPath, "/mnt/sdcard");
  pstHandle->stDevAttr.s32AutoDel = 1;
  pstHandle->stDevAttr.s32FreeSizeDelMin = 500;
  pstHandle->stDevAttr.s32FreeSizeDelMax = 1000;
  pstHandle->stDevAttr.s32FolderNum = 2;
  pstHandle->stDevAttr.pstFolderAttr = (RKADK_STR_FOLDER_ATTR *)malloc(
      sizeof(RKADK_STR_FOLDER_ATTR) * pstHandle->stDevAttr.s32FolderNum);

  if (!pstHandle->stDevAttr.pstFolderAttr) {
    RKADK_LOGE("stDevAttr.pstFolderAttr malloc failed.");
    return -1;
  }
  memset(pstHandle->stDevAttr.pstFolderAttr, 0,
         sizeof(RKADK_STR_FOLDER_ATTR) * pstHandle->stDevAttr.s32FolderNum);

  pstHandle->stDevAttr.pstFolderAttr[0].s32SortCond = SORT_FILE_NAME;
  pstHandle->stDevAttr.pstFolderAttr[0].bNumLimit = RKADK_FALSE;
  pstHandle->stDevAttr.pstFolderAttr[0].s32Limit = 50;
  sprintf(pstHandle->stDevAttr.pstFolderAttr[0].cFolderPath, "/video_front/");
  pstHandle->stDevAttr.pstFolderAttr[1].s32SortCond = SORT_FILE_NAME;
  pstHandle->stDevAttr.pstFolderAttr[1].bNumLimit = RKADK_FALSE;
  pstHandle->stDevAttr.pstFolderAttr[1].s32Limit = 50;
  sprintf(pstHandle->stDevAttr.pstFolderAttr[1].cFolderPath, "/video_back/");

  for (i = 0; i < pstHandle->stDevAttr.s32FolderNum; i++) {
    RKADK_LOGI(
        "DevAttr set:  AutoDel--%d, FreeSizeDel--%d~%d, Path--%s%s, Limit--%d",
        pstHandle->stDevAttr.s32AutoDel, pstHandle->stDevAttr.s32FreeSizeDelMin,
        pstHandle->stDevAttr.s32FreeSizeDelMax, pstHandle->stDevAttr.cMountPath,
        pstHandle->stDevAttr.pstFolderAttr[i].cFolderPath,
        pstHandle->stDevAttr.pstFolderAttr[i].s32Limit);
  }

  return 0;
}

static RKADK_S32 RKADK_STORAGE_ParameterDeinit(RKADK_STORAGE_HANDLE *pHandle) {
  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);

  if (pHandle->stDevAttr.pstFolderAttr) {
    free(pHandle->stDevAttr.pstFolderAttr);
    pHandle->stDevAttr.pstFolderAttr = NULL;
  }

  return 0;
}

static RKADK_S32 RKADK_STORAGE_AutoDeleteInit(RKADK_STORAGE_HANDLE *pstHandle) {
  RKADK_STR_DEV_ATTR stDevAttr;

  RKADK_CHECK_POINTER(pstHandle, RKADK_FAILURE);
  stDevAttr = RKADK_STORAGE_GetParam(pstHandle);

  if (!RKADK_STORAGE_GetMountDev(
          stDevAttr.cMountPath, pstHandle->stDevSta.cDevPath,
          pstHandle->stDevSta.cDevType, pstHandle->stDevSta.cDevAttr1)) {

    pstHandle->stDevSta.s32MountStatus = DISK_MOUNTED;
    if (pthread_create(&(pstHandle->stDevSta.fileScanTid), NULL,
                       RKADK_STORAGE_FileScanThread,
                       (RKADK_MW_PTR)(pstHandle))) {
      RKADK_LOGE("FileScanThread create failed.");
      return -1;
    }
  } else {
    pstHandle->stDevSta.s32MountStatus = DISK_UNMOUNTED;
    RKADK_LOGE("GetMountDev failed.");
    return -1;
  }

  return 0;
}

static RKADK_S32 RKADK_STORAGE_AutoDeleteDeinit(RKADK_STORAGE_HANDLE *pHandle) {
  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);

  pHandle->stDevSta.s32MountStatus = DISK_UNMOUNTED;

  if (pHandle->stDevSta.fileScanTid)
    if (pthread_join(pHandle->stDevSta.fileScanTid, NULL))
      RKADK_LOGE("FileScanThread join failed.");

  return 0;
}

static RKADK_S32 RKADK_STORAGE_ListenMsgInit(RKADK_STORAGE_HANDLE *pstHandle) {
  RKADK_CHECK_POINTER(pstHandle, RKADK_FAILURE);

  pstHandle->eventListenerRun = 1;

  if (RKADK_STORAGE_MsgCreate(&RKADK_STORAGE_MsgRecCb, pstHandle)) {
    RKADK_LOGE("Msg create failed.");
    return -1;
  }

  if (pthread_create(&pstHandle->eventListenerTid, NULL,
                     RKADK_STORAGE_EventListenerThread,
                     (RKADK_MW_PTR)pstHandle)) {
    RKADK_LOGE("EventListenerThread create failed.");
    return -1;
  }

  return 0;
}

RKADK_S32 RKADK_STORAGE_Init(RKADK_MW_PTR *ppHandle,
                             RKADK_STR_DEV_ATTR *pstDevAttr) {
  RKADK_STORAGE_HANDLE *pstHandle = NULL;

  if (*ppHandle) {
    RKADK_LOGE("Storage handle has been inited.");
    return -1;
  }

  pstHandle = (RKADK_STORAGE_HANDLE *)malloc(sizeof(RKADK_STORAGE_HANDLE));
  if (!pstHandle) {
    RKADK_LOGE("pstHandle malloc failed.");
    return -1;
  }
  memset(pstHandle, 0, sizeof(RKADK_STORAGE_HANDLE));

  if (RKADK_STORAGE_ParameterInit(pstHandle, pstDevAttr)) {
    RKADK_LOGE("Parameter init failed.");
    goto failed;
  }

  if (RKADK_STORAGE_AutoDeleteInit(pstHandle))
    RKADK_LOGE("AutoDelete init failed.");

  if (RKADK_STORAGE_ListenMsgInit(pstHandle)) {
    RKADK_LOGE("Listener and Msg init failed.");
    goto failed;
  }

  *ppHandle = (RKADK_MW_PTR)pstHandle;
  return 0;

failed:
  if (pstHandle)
    free(pstHandle);

  return -1;
}

RKADK_S32 RKADK_STORAGE_Deinit(RKADK_MW_PTR pHandle) {
  RKADK_STORAGE_HANDLE *pstHandle = NULL;

  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);
  pstHandle = (RKADK_STORAGE_HANDLE *)pHandle;
  pstHandle->eventListenerRun = 0;

  if (pstHandle->eventListenerTid)
    if (pthread_join(pstHandle->eventListenerTid, NULL))
      RKADK_LOGE("EventListenerThread join failed.");

  if (RKADK_STORAGE_MsgDestroy(pstHandle))
    RKADK_LOGE("Msg destroy failed.");

  if (RKADK_STORAGE_AutoDeleteDeinit(pstHandle))
    RKADK_LOGE("AutoDelete deinit failed.");

  if (RKADK_STORAGE_ParameterDeinit(pstHandle))
    RKADK_LOGE("Paramete deinit failed.");

  free(pstHandle);
  pstHandle = NULL;

  return 0;
}

RKADK_MOUNT_STATUS RKADK_STORAGE_GetMountStatus(RKADK_MW_PTR pHandle) {
  RKADK_STORAGE_HANDLE *pstHandle;

  RKADK_CHECK_POINTER(pHandle, DISK_MOUNT_BUTT);
  pstHandle = (RKADK_STORAGE_HANDLE *)pHandle;
  return pstHandle->stDevSta.s32MountStatus;
}

RKADK_S32 RKADK_STORAGE_GetSdcardSize(RKADK_MW_PTR *ppHandle,
                                      RKADK_S32 *totalSize,
                                      RKADK_S32 *freeSize) {
  RKADK_STORAGE_HANDLE *pstHandle = NULL;
  RKADK_STR_DEV_ATTR stDevAttr;

  RKADK_CHECK_POINTER(ppHandle, RKADK_FAILURE);
  RKADK_CHECK_POINTER(*ppHandle, RKADK_FAILURE);
  RKADK_CHECK_POINTER(totalSize, RKADK_FAILURE);
  RKADK_CHECK_POINTER(freeSize, RKADK_FAILURE);
  pstHandle = (RKADK_STORAGE_HANDLE *)*ppHandle;
  stDevAttr = RKADK_STORAGE_GetParam(pstHandle);

  if (pstHandle->stDevSta.s32MountStatus == DISK_MOUNTED) {
    RKADK_STORAGE_GetDiskSize(stDevAttr.cMountPath,
                              &pstHandle->stDevSta.s32TotalSize,
                              &pstHandle->stDevSta.s32FreeSize);
  } else {
    pstHandle->stDevSta.s32TotalSize = 0;
    pstHandle->stDevSta.s32FreeSize = 0;
  }
  *totalSize = pstHandle->stDevSta.s32TotalSize;
  *freeSize = pstHandle->stDevSta.s32FreeSize;

  *ppHandle = (RKADK_MW_PTR)pstHandle;
  return 0;
}

RKADK_S32 RKADK_STORAGE_GetFileList(RKADK_FILE_LIST *list, RKADK_MW_PTR pHandle,
                                    RKADK_SORT_TYPE sort) {
  RKADK_S32 i, j;
  RKADK_STORAGE_HANDLE *pstHandle = NULL;

  RKADK_CHECK_POINTER(list, RKADK_FAILURE);
  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);
  pstHandle = (RKADK_STORAGE_HANDLE *)pHandle;

  for (i = 0; i < pstHandle->stDevSta.s32FolderNum; i++) {
    if (!strcmp(list->path, pstHandle->stDevSta.pstFolder[i].cpath))
      break;
  }

  if (i == pstHandle->stDevSta.s32FolderNum) {
    RKADK_LOGE("No folder found. Please check the folder path.\n");
    return -1;
  }

  pthread_mutex_lock(&pstHandle->stDevSta.pstFolder[i].mutex);

  RKADK_STR_FILE *tmp = pstHandle->stDevSta.pstFolder[i].pstFileListFirst;
  list->s32FileNum = pstHandle->stDevSta.pstFolder[i].s32FileNum;
  list->file =
      (RKADK_FILE_INFO *)malloc(sizeof(RKADK_FILE_INFO) * list->s32FileNum);
  if (!list->file) {
    RKADK_LOGE("list->file malloc failed.");
    return -1;
  }
  memset(list->file, 0, sizeof(RKADK_FILE_INFO) * list->s32FileNum);

  if (sort == LIST_ASCENDING) {
    for (j = 0; j < list->s32FileNum && tmp != NULL; j++) {
      int len = strlen(tmp->filename) > (RKADK_MAX_FILE_PATH_LEN - 1)
                    ? (RKADK_MAX_FILE_PATH_LEN - 1)
                    : strlen(tmp->filename);
      strncpy(list->file[j].filename, tmp->filename, len);
      list->file[j].filename[len] = '\0';
      list->file[j].stSize = tmp->stSize;
      list->file[j].stTime = tmp->stTime;
      tmp = tmp->next;
    }
  } else {
    for (j = list->s32FileNum - 1; j >= 0 && tmp != NULL; j--) {
      int len = strlen(tmp->filename) > (RKADK_MAX_FILE_PATH_LEN - 1)
                    ? (RKADK_MAX_FILE_PATH_LEN - 1)
                    : strlen(tmp->filename);
      strncpy(list->file[j].filename, tmp->filename, len);
      list->file[j].filename[len] = '\0';
      list->file[j].stSize = tmp->stSize;
      list->file[j].stTime = tmp->stTime;
      tmp = tmp->next;
    }
  }

  pthread_mutex_unlock(&pstHandle->stDevSta.pstFolder[i].mutex);
  return 0;
}

RKADK_S32 RKADK_STORAGE_FreeFileList(RKADK_FILE_LIST *list) {
  if (list->file) {
    free(list->file);
    list->file = NULL;
  }

  return 0;
}

RKADK_S32 RKADK_STORAGE_GetFileNum(RKADK_CHAR *fileListPath,
                                   RKADK_MW_PTR pHandle) {
  RKADK_S32 i;
  RKADK_STORAGE_HANDLE *pstHandle = NULL;

  RKADK_CHECK_POINTER(pHandle, RKADK_FAILURE);
  pstHandle = (RKADK_STORAGE_HANDLE *)pHandle;

  for (i = 0; i < pstHandle->stDevSta.s32FolderNum; i++) {
    if (!strcmp(fileListPath, pstHandle->stDevSta.pstFolder[i].cpath))
      break;
  }

  if (i == pstHandle->stDevSta.s32FolderNum)
    return 0;

  return pstHandle->stDevSta.pstFolder[i].s32FileNum;
}

RKADK_CHAR *RKADK_STORAGE_GetDevPath(RKADK_MW_PTR pHandle) {
  RKADK_STORAGE_HANDLE *pstHandle = NULL;

  RKADK_CHECK_POINTER(pHandle, NULL);
  pstHandle = (RKADK_STORAGE_HANDLE *)pHandle;

  return pstHandle->stDevSta.cDevPath;
}
