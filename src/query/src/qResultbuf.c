#include "qResultbuf.h"
#include "stddef.h"
#include "tscompression.h"
#include "hash.h"
#include "qExtbuffer.h"
#include "queryLog.h"
#include "taoserror.h"

#define GET_DATA_PAYLOAD(_p) ((_p)->pData + POINTER_BYTES)

int32_t createDiskbasedResultBuffer(SDiskbasedResultBuf** pResultBuf, int32_t rowSize, int32_t pagesize,
                                    int32_t inMemBufSize, const void* handle) {
  *pResultBuf = calloc(1, sizeof(SDiskbasedResultBuf));

  SDiskbasedResultBuf* pResBuf = *pResultBuf;
  if (pResBuf == NULL) {
    return TSDB_CODE_COM_OUT_OF_MEMORY;  
  }

  pResBuf->pageSize     = pagesize;
  pResBuf->numOfPages   = 0;                        // all pages are in buffer in the first place
  pResBuf->totalBufSize = 0;
  pResBuf->inMemPages   = inMemBufSize/pagesize;    // maximum allowed pages, it is a soft limit.
  pResBuf->allocateId   = -1;
  pResBuf->comp         = true;
  pResBuf->file         = NULL;
  pResBuf->handle       = handle;
  pResBuf->fileSize = 0;

  // at least more than 2 pages must be in memory
  assert(inMemBufSize >= pagesize * 2);

  pResBuf->numOfRowsPerPage = (pagesize - sizeof(tFilePage)) / rowSize;
  pResBuf->lruList = tdListNew(POINTER_BYTES);

  // init id hash table
  pResBuf->groupSet  = taosHashInit(10, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), true, false);
  pResBuf->assistBuf = malloc(pResBuf->pageSize + 2); // EXTRA BYTES
  pResBuf->all = taosHashInit(10, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), true, false);

  char path[PATH_MAX] = {0};
  getTmpfilePath("qbuf", path);
  pResBuf->path = strdup(path);

  pResBuf->emptyDummyIdList = taosArrayInit(1, sizeof(int32_t));

  qDebug("QInfo:%p create resBuf for output, page size:%d, inmem buf pages:%d, file:%s", handle, pResBuf->pageSize,
         pResBuf->inMemPages, pResBuf->path);

  return TSDB_CODE_SUCCESS;
}

static int32_t createDiskFile(SDiskbasedResultBuf* pResultBuf) {
  pResultBuf->file = fopen(pResultBuf->path, "wb+");
  if (pResultBuf->file == NULL) {
    qError("failed to create tmp file: %s on disk. %s", pResultBuf->path, strerror(errno));
    return TAOS_SYSTEM_ERROR(errno);
  }

  return TSDB_CODE_SUCCESS;
}

static char* doCompressData(void* data, int32_t srcSize, int32_t *dst, SDiskbasedResultBuf* pResultBuf) { // do nothing
  if (!pResultBuf->comp) {
    *dst = srcSize;
    return data;
  }

  *dst = tsCompressString(data, srcSize, 1, pResultBuf->assistBuf, srcSize, ONE_STAGE_COMP, NULL, 0);

  memcpy(data, pResultBuf->assistBuf, *dst);
  return data;
}

static char* doDecompressData(void* data, int32_t srcSize, int32_t *dst, SDiskbasedResultBuf* pResultBuf) { // do nothing
  if (!pResultBuf->comp) {
    *dst = srcSize;
    return data;
  }

  *dst = tsDecompressString(data, srcSize, 1, pResultBuf->assistBuf, pResultBuf->pageSize, ONE_STAGE_COMP, NULL, 0);

  memcpy(data, pResultBuf->assistBuf, *dst);
  return data;
}

static int32_t allocatePositionInFile(SDiskbasedResultBuf* pResultBuf, size_t size) {
  if (pResultBuf->pFree == NULL) {
    return pResultBuf->nextPos;
  } else {
    int32_t offset = -1;

    size_t num = taosArrayGetSize(pResultBuf->pFree);
    for(int32_t i = 0; i < num; ++i) {
      SFreeListItem* pi = taosArrayGet(pResultBuf->pFree, i);
      if (pi->len >= size) {
        offset = pi->offset;
        pi->offset += size;
        pi->len -= size;

        return offset;
      }
    }

    // no available recycle space, allocate new area in file
    return pResultBuf->nextPos;
  }
}

static char* doFlushPageToDisk(SDiskbasedResultBuf* pResultBuf, SPageInfo* pg) {
  assert(!pg->used && pg->pData != NULL);

  int32_t size = -1;
  char* t = doCompressData(GET_DATA_PAYLOAD(pg), pResultBuf->pageSize, &size, pResultBuf);

  // this page is flushed to disk for the first time
  if (pg->info.offset == -1) {
    pg->info.offset = allocatePositionInFile(pResultBuf, size);
    pResultBuf->nextPos += size;

    fseek(pResultBuf->file, pg->info.offset, SEEK_SET);
    /*int32_t ret =*/ fwrite(t, 1, size, pResultBuf->file);

    if (pResultBuf->fileSize < pg->info.offset + pg->info.length) {
      pResultBuf->fileSize = pg->info.offset + pg->info.length;
    }
  } else {
    // length becomes greater, current space is not enough, allocate new place, otherwise, do nothing
    if (pg->info.length < size) {
      // 1. add current space to free list
      taosArrayPush(pResultBuf->pFree, &pg->info);

      // 2. allocate new position, and update the info
      pg->info.offset = allocatePositionInFile(pResultBuf, size);
      pResultBuf->nextPos += size;
    }

    //3. write to disk.
    fseek(pResultBuf->file, pg->info.offset, SEEK_SET);
    fwrite(t, size, 1, pResultBuf->file);

    if (pResultBuf->fileSize < pg->info.offset + pg->info.length) {
      pResultBuf->fileSize = pg->info.offset + pg->info.length;
    }
  }

  char* ret = pg->pData;
  memset(ret, 0, pResultBuf->pageSize);

  pg->pData = NULL;
  pg->info.length = size;

  pResultBuf->statis.flushBytes += pg->info.length;

  return ret;
}

static char* flushPageToDisk(SDiskbasedResultBuf* pResultBuf, SPageInfo* pg) {
  int32_t ret = TSDB_CODE_SUCCESS;
  assert(pResultBuf->numOfPages * pResultBuf->pageSize == pResultBuf->totalBufSize && pResultBuf->numOfPages >= pResultBuf->inMemPages);

  if (pResultBuf->file == NULL) {
    if ((ret = createDiskFile(pResultBuf)) != TSDB_CODE_SUCCESS) {
      terrno = ret;
      return NULL;
    }
  }

  return doFlushPageToDisk(pResultBuf, pg);
}

// load file block data in disk
static char* loadPageFromDisk(SDiskbasedResultBuf* pResultBuf, SPageInfo* pg) {
  int32_t ret = fseek(pResultBuf->file, pg->info.offset, SEEK_SET);
  ret = fread(GET_DATA_PAYLOAD(pg), 1, pg->info.length, pResultBuf->file);
  if (ret != pg->info.length) {
    terrno = errno;
    return NULL;
  }

  pResultBuf->statis.loadBytes += pg->info.length;

  int32_t fullSize = 0;
  doDecompressData(GET_DATA_PAYLOAD(pg), pg->info.length, &fullSize, pResultBuf);

  return GET_DATA_PAYLOAD(pg);
}

#define NO_AVAILABLE_PAGES(_b) ((_b)->numOfPages >= (_b)->inMemPages)

static SIDList addNewGroup(SDiskbasedResultBuf* pResultBuf, int32_t groupId) {
  assert(taosHashGet(pResultBuf->groupSet, (const char*) &groupId, sizeof(int32_t)) == NULL);

  SArray* pa = taosArrayInit(1, POINTER_BYTES);
  int32_t ret = taosHashPut(pResultBuf->groupSet, (const char*)&groupId, sizeof(int32_t), &pa, POINTER_BYTES);
  assert(ret == 0);

  return pa;
}

static SPageInfo* registerPage(SDiskbasedResultBuf* pResultBuf, int32_t groupId, int32_t pageId) {
  SIDList list = NULL;

  char** p = taosHashGet(pResultBuf->groupSet, (const char*)&groupId, sizeof(int32_t));
  if (p == NULL) {  // it is a new group id
    list = addNewGroup(pResultBuf, groupId);
  } else {
    list = (SIDList) (*p);
  }

  pResultBuf->numOfPages += 1;

  SPageInfo* ppi = malloc(sizeof(SPageInfo));//{ .info = PAGE_INFO_INITIALIZER, .pageId = pageId, .pn = NULL};
  ppi->info     = PAGE_INFO_INITIALIZER;
  ppi->pageId   = pageId;
  ppi->pData    = NULL;
  ppi->pn       = NULL;
  ppi->used     = true;

  return *(SPageInfo**) taosArrayPush(list, &ppi);
}

static SListNode* getEldestUnrefedPage(SDiskbasedResultBuf* pResultBuf) {
  SListIter iter = {0};
  tdListInitIter(pResultBuf->lruList, &iter, TD_LIST_BACKWARD);

  SListNode* pn = NULL;
  while((pn = tdListNext(&iter)) != NULL) {
    assert(pn != NULL);

    SPageInfo* pageInfo = *(SPageInfo**) pn->data;
    assert(pageInfo->pageId >= 0 && pageInfo->pn == pn);

    if (!pageInfo->used) {
      break;
    }
  }

  return pn;
}

static char* evicOneDataPage(SDiskbasedResultBuf* pResultBuf) {
  char* bufPage = NULL;
  SListNode* pn = getEldestUnrefedPage(pResultBuf);

  // all pages are referenced by user, try to allocate new space
  if (pn == NULL) {
    int32_t prev = pResultBuf->inMemPages;
    pResultBuf->inMemPages = pResultBuf->inMemPages * 1.5;

    qWarn("%p in memory buf page not sufficient, expand from %d to %d, page size:%d", pResultBuf, prev,
          pResultBuf->inMemPages, pResultBuf->pageSize);
  } else {
    pResultBuf->statis.flushPages += 1;
    tdListPopNode(pResultBuf->lruList, pn);

    SPageInfo* d = *(SPageInfo**) pn->data;
    assert(d->pn == pn);

    d->pn = NULL;
    tfree(pn);

    bufPage = flushPageToDisk(pResultBuf, d);
  }

  return bufPage;
}

static void lruListPushFront(SList *pList, SPageInfo* pi) {
  tdListPrepend(pList, &pi);
  SListNode* front = tdListGetHead(pList);
  pi->pn = front;
}

static void lruListMoveToFront(SList *pList, SPageInfo* pi) {
  tdListPopNode(pList, pi->pn);
  tdListPrependNode(pList, pi->pn);
}

tFilePage* getNewDataBuf(SDiskbasedResultBuf* pResultBuf, int32_t groupId, int32_t* pageId) {
  pResultBuf->statis.getPages += 1;

  char* availablePage = NULL;
  if (NO_AVAILABLE_PAGES(pResultBuf)) {
    availablePage = evicOneDataPage(pResultBuf);
  }

  // register new id in this group
  *pageId = (++pResultBuf->allocateId);

  // register page id info
  SPageInfo* pi = registerPage(pResultBuf, groupId, *pageId);

  // add to LRU list
  assert(listNEles(pResultBuf->lruList) < pResultBuf->inMemPages && pResultBuf->inMemPages > 0);

  lruListPushFront(pResultBuf->lruList, pi);

  // add to hash map
  taosHashPut(pResultBuf->all, pageId, sizeof(int32_t), &pi, POINTER_BYTES);

  // allocate buf
  if (availablePage == NULL) {
    pi->pData = calloc(1, pResultBuf->pageSize + POINTER_BYTES);
  } else {
    pi->pData = availablePage;
  }

  pResultBuf->totalBufSize += pResultBuf->pageSize;

  ((void**)pi->pData)[0] = pi;
  pi->used = true;

  return GET_DATA_PAYLOAD(pi);
}

tFilePage* getResBufPage(SDiskbasedResultBuf* pResultBuf, int32_t id) {
  assert(pResultBuf != NULL && id >= 0);
  pResultBuf->statis.getPages += 1;

  SPageInfo** pi = taosHashGet(pResultBuf->all, &id, sizeof(int32_t));
  assert(pi != NULL && *pi != NULL);

  if ((*pi)->pData != NULL) { // it is in memory
    // no need to update the LRU list if only one page exists
    if (pResultBuf->numOfPages == 1) {
      (*pi)->used = true;
      return GET_DATA_PAYLOAD(*pi);
    }

    SPageInfo** pInfo = (SPageInfo**) ((*pi)->pn->data);
    assert(*pInfo == *pi);

    lruListMoveToFront(pResultBuf->lruList, (*pi));
    (*pi)->used = true;

    return GET_DATA_PAYLOAD(*pi);

  } else { // not in memory
    assert((*pi)->pData == NULL && (*pi)->pn == NULL && (*pi)->info.length >= 0 && (*pi)->info.offset >= 0);

    char* availablePage = NULL;
    if (NO_AVAILABLE_PAGES(pResultBuf)) {
      availablePage = evicOneDataPage(pResultBuf);
    }

    if (availablePage == NULL) {
      (*pi)->pData = calloc(1, pResultBuf->pageSize + POINTER_BYTES);
    } else {
      (*pi)->pData = availablePage;
    }

    ((void**)((*pi)->pData))[0] = (*pi);

    lruListPushFront(pResultBuf->lruList, *pi);
    loadPageFromDisk(pResultBuf, *pi);
    return GET_DATA_PAYLOAD(*pi);
  }
}

void releaseResBufPage(SDiskbasedResultBuf* pResultBuf, void* page) {
  assert(pResultBuf != NULL && page != NULL);
  char* p = (char*) page - POINTER_BYTES;

  SPageInfo* ppi = ((SPageInfo**) p)[0];
  releaseResBufPageInfo(pResultBuf, ppi);
}

void releaseResBufPageInfo(SDiskbasedResultBuf* pResultBuf, SPageInfo* pi) {
  assert(pi->pData != NULL && pi->used);

  pi->used = false;
  pResultBuf->statis.releasePages += 1;
}

size_t getNumOfRowsPerPage(const SDiskbasedResultBuf* pResultBuf) { return pResultBuf->numOfRowsPerPage; }

size_t getNumOfResultBufGroupId(const SDiskbasedResultBuf* pResultBuf) { return taosHashGetSize(pResultBuf->groupSet); }

size_t getResBufSize(const SDiskbasedResultBuf* pResultBuf) { return pResultBuf->totalBufSize; }

SIDList getDataBufPagesIdList(SDiskbasedResultBuf* pResultBuf, int32_t groupId) {
  assert(pResultBuf != NULL);

  char** p = taosHashGet(pResultBuf->groupSet, (const char*)&groupId, sizeof(int32_t));
  if (p == NULL) {  // it is a new group id
    return pResultBuf->emptyDummyIdList;
  } else {
    return (SArray*) (*p);
  }
}

void destroyResultBuf(SDiskbasedResultBuf* pResultBuf) {
  if (pResultBuf == NULL) {
    return;
  }

  if (pResultBuf->file != NULL) {
    qDebug("QInfo:%p disk-based output buffer closed, total:%" PRId64 " bytes, file size:%"PRId64" bytes",
        pResultBuf->handle, pResultBuf->totalBufSize, pResultBuf->fileSize);

    fclose(pResultBuf->file);
  } else {
    qDebug("QInfo:%p disk-based output buffer closed, total:%" PRId64 " bytes, no file created", pResultBuf->handle,
           pResultBuf->totalBufSize);
  }

  unlink(pResultBuf->path);
  tfree(pResultBuf->path);

  SHashMutableIterator* iter = taosHashCreateIter(pResultBuf->groupSet);
  while(taosHashIterNext(iter)) {
    SArray** p = (SArray**) taosHashIterGet(iter);
    size_t n = taosArrayGetSize(*p);
    for(int32_t i = 0; i < n; ++i) {
      SPageInfo* pi = taosArrayGetP(*p, i);
      tfree(pi->pData);
      tfree(pi);
    }

    taosArrayDestroy(*p);
  }

  taosHashDestroyIter(iter);

  tdListFree(pResultBuf->lruList);
  taosArrayDestroy(pResultBuf->emptyDummyIdList);
  taosHashCleanup(pResultBuf->groupSet);
  taosHashCleanup(pResultBuf->all);

  tfree(pResultBuf->assistBuf);
  tfree(pResultBuf);
}

SPageInfo* getLastPageInfo(SIDList pList) {
  size_t size = taosArrayGetSize(pList);
  return (SPageInfo*) taosArrayGetP(pList, size - 1);
}

