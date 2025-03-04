/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "parInsertUtil.h"
#include "parToken.h"
#include "scalar.h"
#include "tglobal.h"
#include "ttime.h"
#include "geosWrapper.h"

#define NEXT_TOKEN_WITH_PREV(pSql, token)           \
  do {                                              \
    int32_t index = 0;                              \
    token = tStrGetToken(pSql, &index, true, NULL); \
    pSql += index;                                  \
  } while (0)

#define NEXT_TOKEN_WITH_PREV_EXT(pSql, token, pIgnoreComma) \
  do {                                                      \
    int32_t index = 0;                                      \
    token = tStrGetToken(pSql, &index, true, pIgnoreComma); \
    pSql += index;                                          \
  } while (0)

#define NEXT_TOKEN_KEEP_SQL(pSql, token, index)      \
  do {                                               \
    token = tStrGetToken(pSql, &index, false, NULL); \
  } while (0)

#define NEXT_VALID_TOKEN(pSql, token)           \
  do {                                          \
    (token).n = tGetToken(pSql, &(token).type); \
    (token).z = (char*)pSql;                    \
    pSql += (token).n;                          \
  } while (TK_NK_SPACE == (token).type)

typedef struct SInsertParseContext {
  SParseContext* pComCxt;
  SMsgBuf        msg;
  char           tmpTokenBuf[TSDB_MAX_BYTES_PER_ROW];
  SBoundColInfo  tags;  // for stmt
  bool           missCache;
  bool           usingDuplicateTable;
  bool           forceUpdate;
  bool           needTableTagVal;
} SInsertParseContext;

typedef int32_t (*_row_append_fn_t)(SMsgBuf* pMsgBuf, const void* value, int32_t len, void* param);

static uint8_t TRUE_VALUE = (uint8_t)TSDB_TRUE;
static uint8_t FALSE_VALUE = (uint8_t)TSDB_FALSE;

static bool isNullStr(SToken* pToken) {
  return ((pToken->type == TK_NK_STRING) && (strlen(TSDB_DATA_NULL_STR_L) == pToken->n) &&
          (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0));
}

static bool isNullValue(int8_t dataType, SToken* pToken) {
  return TK_NULL == pToken->type || (!IS_STR_DATA_TYPE(dataType) && isNullStr(pToken));
}

static FORCE_INLINE int32_t toDouble(SToken* pToken, double* value, char** endPtr) {
  errno = 0;
  *value = taosStr2Double(pToken->z, endPtr);

  // not a valid integer number, return error
  if ((*endPtr - pToken->z) != pToken->n) {
    return TK_NK_ILLEGAL;
  }

  return pToken->type;
}

static int32_t skipInsertInto(const char** pSql, SMsgBuf* pMsg) {
  SToken token;
  NEXT_TOKEN(*pSql, token);
  if (TK_INSERT != token.type && TK_IMPORT != token.type) {
    return buildSyntaxErrMsg(pMsg, "keyword INSERT is expected", token.z);
  }
  NEXT_TOKEN(*pSql, token);
  if (TK_INTO != token.type) {
    return buildSyntaxErrMsg(pMsg, "keyword INTO is expected", token.z);
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t skipParentheses(SInsertParseContext* pCxt, const char** pSql) {
  SToken  token;
  int32_t expectRightParenthesis = 1;
  while (1) {
    NEXT_TOKEN(*pSql, token);
    if (TK_NK_LP == token.type) {
      ++expectRightParenthesis;
    } else if (TK_NK_RP == token.type && 0 == --expectRightParenthesis) {
      break;
    }
    if (0 == token.n) {
      return buildSyntaxErrMsg(&pCxt->msg, ") expected", NULL);
    }
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t skipTableOptions(SInsertParseContext* pCxt, const char** pSql) {
  do {
    int32_t index = 0;
    SToken  token;
    NEXT_TOKEN_KEEP_SQL(*pSql, token, index);
    if (TK_TTL == token.type || TK_COMMENT == token.type) {
      *pSql += index;
      NEXT_TOKEN_WITH_PREV(*pSql, token);
    } else {
      break;
    }
  } while (1);
  return TSDB_CODE_SUCCESS;
}

// pSql -> stb_name [(tag1_name, ...)] TAGS (tag1_value, ...)
static int32_t ignoreUsingClause(SInsertParseContext* pCxt, const char** pSql) {
  int32_t code = TSDB_CODE_SUCCESS;
  SToken  token;
  NEXT_TOKEN(*pSql, token);

  NEXT_TOKEN(*pSql, token);
  if (TK_NK_LP == token.type) {
    code = skipParentheses(pCxt, pSql);
    if (TSDB_CODE_SUCCESS == code) {
      NEXT_TOKEN(*pSql, token);
    }
  }

  // pSql -> TAGS (tag1_value, ...)
  if (TSDB_CODE_SUCCESS == code) {
    if (TK_TAGS != token.type) {
      code = buildSyntaxErrMsg(&pCxt->msg, "TAGS is expected", token.z);
    } else {
      NEXT_TOKEN(*pSql, token);
    }
  }
  if (TSDB_CODE_SUCCESS == code) {
    if (TK_NK_LP != token.type) {
      code = buildSyntaxErrMsg(&pCxt->msg, "( is expected", token.z);
    } else {
      code = skipParentheses(pCxt, pSql);
    }
  }

  if (TSDB_CODE_SUCCESS == code) {
    code = skipTableOptions(pCxt, pSql);
  }

  return code;
}

static int32_t parseDuplicateUsingClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, bool* pDuplicate) {
  *pDuplicate = false;

  char tbFName[TSDB_TABLE_FNAME_LEN];
  tNameExtractFullName(&pStmt->targetTableName, tbFName);
  STableMeta** pMeta = taosHashGet(pStmt->pSubTableHashObj, tbFName, strlen(tbFName));
  if (NULL != pMeta) {
    *pDuplicate = true;
    int32_t code = ignoreUsingClause(pCxt, &pStmt->pSql);
    if (TSDB_CODE_SUCCESS == code) {
      return cloneTableMeta(*pMeta, &pStmt->pTableMeta);
    }
  }

  return TSDB_CODE_SUCCESS;
}

// pStmt->pSql -> field1_name, ...)
static int32_t parseBoundColumns(SInsertParseContext* pCxt, const char** pSql, bool isTags, SSchema* pSchema,
                                 SBoundColInfo* pBoundInfo) {
  bool* pUseCols = taosMemoryCalloc(pBoundInfo->numOfCols, sizeof(bool));
  if (NULL == pUseCols) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  pBoundInfo->numOfBound = 0;

  int16_t lastColIdx = -1;  // last column found
  int32_t code = TSDB_CODE_SUCCESS;
  while (TSDB_CODE_SUCCESS == code) {
    SToken token;
    NEXT_TOKEN(*pSql, token);

    if (TK_NK_RP == token.type) {
      break;
    }

    char tmpTokenBuf[TSDB_COL_NAME_LEN + 2] = {0};  // used for deleting Escape character backstick(`)
    strncpy(tmpTokenBuf, token.z, token.n);
    token.z = tmpTokenBuf;
    token.n = strdequote(token.z);

    int16_t t = lastColIdx + 1;
    int16_t index = insFindCol(&token, t, pBoundInfo->numOfCols, pSchema);
    if (index < 0 && t > 0) {
      index = insFindCol(&token, 0, t, pSchema);
    }
    if (index < 0) {
      code = generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_INVALID_COLUMN, token.z);
    } else if (pUseCols[index]) {
      code = buildSyntaxErrMsg(&pCxt->msg, "duplicated column name", token.z);
    } else {
      lastColIdx = index;
      pUseCols[index] = true;
      pBoundInfo->pColIndex[pBoundInfo->numOfBound] = index;
      ++pBoundInfo->numOfBound;
    }
  }

  if (TSDB_CODE_SUCCESS == code && !isTags && !pUseCols[0]) {
    code = buildInvalidOperationMsg(&pCxt->msg, "primary timestamp column can not be null");
  }

  taosMemoryFree(pUseCols);

  return code;
}

static int parseTime(const char** end, SToken* pToken, int16_t timePrec, int64_t* time, SMsgBuf* pMsgBuf) {
  int32_t     index = 0;
  int64_t     interval;
  int64_t     ts = 0;
  const char* pTokenEnd = *end;

  if (pToken->type == TK_NOW) {
    ts = taosGetTimestamp(timePrec);
  } else if (pToken->type == TK_TODAY) {
    ts = taosGetTimestampToday(timePrec);
  } else if (pToken->type == TK_NK_INTEGER) {
    if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &ts)) {
      return buildSyntaxErrMsg(pMsgBuf, "invalid timestamp format", pToken->z);
    }
  } else {  // parse the RFC-3339/ISO-8601 timestamp format string
    if (taosParseTime(pToken->z, time, pToken->n, timePrec, tsDaylight) != TSDB_CODE_SUCCESS) {
      return buildSyntaxErrMsg(pMsgBuf, "invalid timestamp format", pToken->z);
    }

    return TSDB_CODE_SUCCESS;
  }

  for (int k = pToken->n; pToken->z[k] != '\0'; k++) {
    if (pToken->z[k] == ' ' || pToken->z[k] == '\t') continue;
    if (pToken->z[k] == '(' && pToken->z[k + 1] == ')') {  // for insert NOW()/TODAY()
      *end = pTokenEnd = &pToken->z[k + 2];
      k++;
      continue;
    }
    if (pToken->z[k] == ',') {
      *end = pTokenEnd;
      *time = ts;
      return 0;
    }

    break;
  }

  /*
   * time expression:
   * e.g., now+12a, now-5h
   */
  index = 0;
  SToken token = tStrGetToken(pTokenEnd, &index, false, NULL);
  pTokenEnd += index;

  if (token.type == TK_NK_MINUS || token.type == TK_NK_PLUS) {
    index = 0;
    SToken valueToken = tStrGetToken(pTokenEnd, &index, false, NULL);
    pTokenEnd += index;

    if (valueToken.n < 2) {
      return buildSyntaxErrMsg(pMsgBuf, "value expected in timestamp", token.z);
    }

    char unit = 0;
    if (parseAbsoluteDuration(valueToken.z, valueToken.n, &interval, &unit, timePrec) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    if (token.type == TK_NK_PLUS) {
      ts += interval;
    } else {
      ts = ts - interval;
    }

    *end = pTokenEnd;
  }

  *time = ts;
  return TSDB_CODE_SUCCESS;
}

// need to call geosFreeBuffer(*output) later
static int parseGeometry(SToken *pToken, unsigned char **output, size_t *size) {
  int32_t code = TSDB_CODE_FAILED;

  //[ToDo] support to parse WKB as well as WKT
  if (pToken->type == TK_NK_STRING) {
    code = initCtxGeomFromText();
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }

    code = doGeomFromText(pToken->z, output, size);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  }

  return code;
}

static int32_t parseVarbinary(SToken* pToken, uint8_t **pData, uint32_t *nData, int32_t bytes){
  if(pToken->type != TK_NK_STRING){
    return TSDB_CODE_PAR_INVALID_VARBINARY;
  }

  if(isHex(pToken->z, pToken->n)){
    if(!isValidateHex(pToken->z, pToken->n)){
      return TSDB_CODE_PAR_INVALID_VARBINARY;
    }

    void* data = NULL;
    uint32_t size = 0;
    if(taosHex2Ascii(pToken->z, pToken->n, &data, &size) < 0){
      return TSDB_CODE_OUT_OF_MEMORY;
    }

    if (size + VARSTR_HEADER_SIZE > bytes) {
      taosMemoryFree(data);
      return TSDB_CODE_PAR_VALUE_TOO_LONG;
    }
    *pData = data;
    *nData = size;
  }else{
    if (pToken->n + VARSTR_HEADER_SIZE > bytes) {
      return TSDB_CODE_PAR_VALUE_TOO_LONG;
    }
    *pData = taosStrdup(pToken->z);
    *nData = pToken->n;
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t parseTagToken(const char** end, SToken* pToken, SSchema* pSchema, int16_t timePrec, STagVal* val,
                             SMsgBuf* pMsgBuf) {
  int64_t  iv;
  uint64_t uv;
  char*    endptr = NULL;
  int32_t  code = TSDB_CODE_SUCCESS;

  if (isNullValue(pSchema->type, pToken)) {
    if (TSDB_DATA_TYPE_TIMESTAMP == pSchema->type && PRIMARYKEY_TIMESTAMP_COL_ID == pSchema->colId) {
      return buildSyntaxErrMsg(pMsgBuf, "primary timestamp should not be null", pToken->z);
    }

    return TSDB_CODE_SUCCESS;
  }

  //  strcpy(val->colName, pSchema->name);
  val->cid = pSchema->colId;
  val->type = pSchema->type;

  switch (pSchema->type) {
    case TSDB_DATA_TYPE_BOOL: {
      if ((pToken->type == TK_NK_BOOL || pToken->type == TK_NK_STRING) && (pToken->n != 0)) {
        if (strncmp(pToken->z, "true", pToken->n) == 0) {
          *(int8_t*)(&val->i64) = TRUE_VALUE;
        } else if (strncmp(pToken->z, "false", pToken->n) == 0) {
          *(int8_t*)(&val->i64) = FALSE_VALUE;
        } else {
          return buildSyntaxErrMsg(pMsgBuf, "invalid bool data", pToken->z);
        }
      } else if (pToken->type == TK_NK_INTEGER) {
        *(int8_t*)(&val->i64) = ((taosStr2Int64(pToken->z, NULL, 10) == 0) ? FALSE_VALUE : TRUE_VALUE);
      } else if (pToken->type == TK_NK_FLOAT) {
        *(int8_t*)(&val->i64) = ((taosStr2Double(pToken->z, NULL) == 0) ? FALSE_VALUE : TRUE_VALUE);
      } else {
        return buildSyntaxErrMsg(pMsgBuf, "invalid bool data", pToken->z);
      }
      break;
    }

    case TSDB_DATA_TYPE_TINYINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid tinyint data", pToken->z);
      } else if (!IS_VALID_TINYINT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "tinyint data overflow", pToken->z);
      }

      *(int8_t*)(&val->i64) = iv;
      break;
    }

    case TSDB_DATA_TYPE_UTINYINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned tinyint data", pToken->z);
      } else if (uv > UINT8_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned tinyint data overflow", pToken->z);
      }
      *(uint8_t*)(&val->i64) = uv;
      break;
    }

    case TSDB_DATA_TYPE_SMALLINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid smallint data", pToken->z);
      } else if (!IS_VALID_SMALLINT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "smallint data overflow", pToken->z);
      }
      *(int16_t*)(&val->i64) = iv;
      break;
    }

    case TSDB_DATA_TYPE_USMALLINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned smallint data", pToken->z);
      } else if (uv > UINT16_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned smallint data overflow", pToken->z);
      }
      *(uint16_t*)(&val->i64) = uv;
      break;
    }

    case TSDB_DATA_TYPE_INT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid int data", pToken->z);
      } else if (!IS_VALID_INT(iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "int data overflow", pToken->z);
      }
      *(int32_t*)(&val->i64) = iv;
      break;
    }

    case TSDB_DATA_TYPE_UINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned int data", pToken->z);
      } else if (uv > UINT32_MAX) {
        return buildSyntaxErrMsg(pMsgBuf, "unsigned int data overflow", pToken->z);
      }
      *(uint32_t*)(&val->i64) = uv;
      break;
    }

    case TSDB_DATA_TYPE_BIGINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &iv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid bigint data", pToken->z);
      }
      val->i64 = iv;
      break;
    }

    case TSDB_DATA_TYPE_UBIGINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &uv)) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid unsigned bigint data", pToken->z);
      }
      *(uint64_t*)(&val->i64) = uv;
      break;
    }

    case TSDB_DATA_TYPE_FLOAT: {
      double dv;
      if (TK_NK_ILLEGAL == toDouble(pToken, &dv, &endptr)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal float data", pToken->z);
      }
      if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || dv > FLT_MAX || dv < -FLT_MAX || isinf(dv) ||
          isnan(dv)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal float data", pToken->z);
      }
      *(float*)(&val->i64) = dv;
      break;
    }

    case TSDB_DATA_TYPE_DOUBLE: {
      double dv;
      if (TK_NK_ILLEGAL == toDouble(pToken, &dv, &endptr)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal double data", pToken->z);
      }
      if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || isinf(dv) || isnan(dv)) {
        return buildSyntaxErrMsg(pMsgBuf, "illegal double data", pToken->z);
      }

      *(double*)(&val->i64) = dv;
      break;
    }

    case TSDB_DATA_TYPE_BINARY: {
      // Too long values will raise the invalid sql error message
      if (pToken->n + VARSTR_HEADER_SIZE > pSchema->bytes) {
        return generateSyntaxErrMsg(pMsgBuf, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
      }
      val->pData = taosStrdup(pToken->z);
      val->nData = pToken->n;
      break;
    }
    case TSDB_DATA_TYPE_VARBINARY: {
      code = parseVarbinary(pToken, &val->pData, &val->nData, pSchema->bytes);
      if(code != TSDB_CODE_SUCCESS){
        return generateSyntaxErrMsg(pMsgBuf, code, pSchema->name);
      }
      break;
    }
    case TSDB_DATA_TYPE_GEOMETRY: {
      unsigned char* output = NULL;
      size_t         size = 0;

      code = parseGeometry(pToken, &output, &size);
      if (code != TSDB_CODE_SUCCESS) {
        code = buildSyntaxErrMsg(pMsgBuf, getThreadLocalGeosCtx()->errMsg, pToken->z);
      } else if (size + VARSTR_HEADER_SIZE > pSchema->bytes) {
        // Too long values will raise the invalid sql error message
        code = generateSyntaxErrMsg(pMsgBuf, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
      } else {
        val->pData = taosMemoryMalloc(size);
        if (NULL == val->pData) {
          code = TSDB_CODE_OUT_OF_MEMORY;
        } else {
          memcpy(val->pData, output, size);
          val->nData = size;
        }
      }

      geosFreeBuffer(output);
      break;
    }

    case TSDB_DATA_TYPE_NCHAR: {
      int32_t output = 0;
      void*   p = taosMemoryCalloc(1, pSchema->bytes - VARSTR_HEADER_SIZE);
      if (p == NULL) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }
      if (!taosMbsToUcs4(pToken->z, pToken->n, (TdUcs4*)(p), pSchema->bytes - VARSTR_HEADER_SIZE, &output)) {
        if (errno == E2BIG) {
          taosMemoryFree(p);
          return generateSyntaxErrMsg(pMsgBuf, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
        }
        char buf[512] = {0};
        snprintf(buf, tListLen(buf), " taosMbsToUcs4 error:%s", strerror(errno));
        taosMemoryFree(p);
        return buildSyntaxErrMsg(pMsgBuf, buf, pToken->z);
      }
      val->pData = p;
      val->nData = output;
      break;
    }
    case TSDB_DATA_TYPE_TIMESTAMP: {
      if (parseTime(end, pToken, timePrec, &iv, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return buildSyntaxErrMsg(pMsgBuf, "invalid timestamp", pToken->z);
      }

      val->i64 = iv;
      break;
    }
  }

  return code;
}

// input pStmt->pSql:  [(tag1_name, ...)] TAGS (tag1_value, ...) ...
// output pStmt->pSql: TAGS (tag1_value, ...) ...
static int32_t parseBoundTagsClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  insInitBoundColsInfo(getNumOfTags(pStmt->pTableMeta), &pCxt->tags);

  SToken  token;
  int32_t index = 0;
  NEXT_TOKEN_KEEP_SQL(pStmt->pSql, token, index);
  if (TK_NK_LP != token.type) {
    return TSDB_CODE_SUCCESS;
  }

  pStmt->pSql += index;
  return parseBoundColumns(pCxt, &pStmt->pSql, true, getTableTagSchema(pStmt->pTableMeta), &pCxt->tags);
}

static int32_t parseTagValue(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, SSchema* pTagSchema, SToken* pToken,
                             SArray* pTagName, SArray* pTagVals, STag** pTag) {
  if (!isNullValue(pTagSchema->type, pToken)) {
    taosArrayPush(pTagName, pTagSchema->name);
  }

  if (pTagSchema->type == TSDB_DATA_TYPE_JSON) {
    if (pToken->n > (TSDB_MAX_JSON_TAG_LEN - VARSTR_HEADER_SIZE) / TSDB_NCHAR_SIZE) {
      return buildSyntaxErrMsg(&pCxt->msg, "json string too long than 4095", pToken->z);
    }

    if (isNullValue(pTagSchema->type, pToken)) {
      return tTagNew(pTagVals, 1, true, pTag);
    } else {
      return parseJsontoTagData(pToken->z, pTagVals, pTag, &pCxt->msg);
    }
  }

  STagVal val = {0};
  int32_t code =
      parseTagToken(&pStmt->pSql, pToken, pTagSchema, pStmt->pTableMeta->tableInfo.precision, &val, &pCxt->msg);
  if (TSDB_CODE_SUCCESS == code) {
    taosArrayPush(pTagVals, &val);
  }

  return code;
}

static int32_t buildCreateTbReq(SVnodeModifyOpStmt* pStmt, STag* pTag, SArray* pTagName) {
  pStmt->pCreateTblReq = taosMemoryCalloc(1, sizeof(SVCreateTbReq));
  if (NULL == pStmt->pCreateTblReq) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  insBuildCreateTbReq(pStmt->pCreateTblReq, pStmt->targetTableName.tname, pTag, pStmt->pTableMeta->suid,
                      pStmt->usingTableName.tname, pTagName, pStmt->pTableMeta->tableInfo.numOfTags,
                      TSDB_DEFAULT_TABLE_TTL);
  return TSDB_CODE_SUCCESS;
}

static int32_t checkAndTrimValue(SToken* pToken, char* tmpTokenBuf, SMsgBuf* pMsgBuf) {
  if ((pToken->type != TK_NOW && pToken->type != TK_TODAY && pToken->type != TK_NK_INTEGER &&
       pToken->type != TK_NK_STRING && pToken->type != TK_NK_FLOAT && pToken->type != TK_NK_BOOL &&
       pToken->type != TK_NULL && pToken->type != TK_NK_HEX && pToken->type != TK_NK_OCT &&
       pToken->type != TK_NK_BIN) ||
      (pToken->n == 0) || (pToken->type == TK_NK_RP)) {
    return buildSyntaxErrMsg(pMsgBuf, "invalid data or symbol", pToken->z);
  }

  // Remove quotation marks
  if (TK_NK_STRING == pToken->type) {
    if (pToken->n >= TSDB_MAX_BYTES_PER_ROW) {
      return buildSyntaxErrMsg(pMsgBuf, "too long string", pToken->z);
    }

    int32_t len = trimString(pToken->z, pToken->n, tmpTokenBuf, TSDB_MAX_BYTES_PER_ROW);
    pToken->z = tmpTokenBuf;
    pToken->n = len;
  }

  return TSDB_CODE_SUCCESS;
}

typedef struct SRewriteTagCondCxt {
  SArray* pTagVals;
  SArray* pTagName;
  int32_t code;
} SRewriteTagCondCxt;

static int32_t rewriteTagCondColumnImpl(STagVal* pVal, SNode** pNode) {
  SValueNode* pValue = (SValueNode*)nodesMakeNode(QUERY_NODE_VALUE);
  if (NULL == pValue) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  pValue->node.resType = ((SColumnNode*)*pNode)->node.resType;
  nodesDestroyNode(*pNode);
  *pNode = (SNode*)pValue;

  switch (pVal->type) {
    case TSDB_DATA_TYPE_BOOL:
      pValue->datum.b = *(int8_t*)(&pVal->i64);
      *(bool*)&pValue->typeData = pValue->datum.b;
      break;
    case TSDB_DATA_TYPE_TINYINT:
      pValue->datum.i = *(int8_t*)(&pVal->i64);
      *(int8_t*)&pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_SMALLINT:
      pValue->datum.i = *(int16_t*)(&pVal->i64);
      *(int16_t*)&pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_INT:
      pValue->datum.i = *(int32_t*)(&pVal->i64);
      *(int32_t*)&pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_BIGINT:
      pValue->datum.i = pVal->i64;
      pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_FLOAT:
      pValue->datum.d = *(float*)(&pVal->i64);
      *(float*)&pValue->typeData = pValue->datum.d;
      break;
    case TSDB_DATA_TYPE_DOUBLE:
      pValue->datum.d = *(double*)(&pVal->i64);
      *(double*)&pValue->typeData = pValue->datum.d;
      break;
    case TSDB_DATA_TYPE_VARCHAR:
    case TSDB_DATA_TYPE_VARBINARY:
    case TSDB_DATA_TYPE_NCHAR:
      pValue->datum.p = taosMemoryCalloc(1, pVal->nData + VARSTR_HEADER_SIZE);
      if (NULL == pValue->datum.p) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }
      varDataSetLen(pValue->datum.p, pVal->nData);
      memcpy(varDataVal(pValue->datum.p), pVal->pData, pVal->nData);
      break;
    case TSDB_DATA_TYPE_TIMESTAMP:
      pValue->datum.i = pVal->i64;
      pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_UTINYINT:
      pValue->datum.i = *(uint8_t*)(&pVal->i64);
      *(uint8_t*)&pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_USMALLINT:
      pValue->datum.i = *(uint16_t*)(&pVal->i64);
      *(uint16_t*)&pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_UINT:
      pValue->datum.i = *(uint32_t*)(&pVal->i64);
      *(uint32_t*)&pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_UBIGINT:
      pValue->datum.i = *(uint64_t*)(&pVal->i64);
      *(uint64_t*)&pValue->typeData = pValue->datum.i;
      break;
    case TSDB_DATA_TYPE_JSON:
    case TSDB_DATA_TYPE_DECIMAL:
    case TSDB_DATA_TYPE_BLOB:
    case TSDB_DATA_TYPE_MEDIUMBLOB:
    default:
      return TSDB_CODE_FAILED;
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t rewriteTagCondColumn(SArray* pTagVals, SArray* pTagName, SNode** pNode) {
  SColumnNode* pCol = (SColumnNode*)*pNode;
  int32_t      ntags = taosArrayGetSize(pTagName);
  for (int32_t i = 0; i < ntags; ++i) {
    char* pTagColName = taosArrayGet(pTagName, i);
    if (0 == strcmp(pTagColName, pCol->colName)) {
      return rewriteTagCondColumnImpl(taosArrayGet(pTagVals, i), pNode);
    }
  }
  return TSDB_CODE_PAR_PERMISSION_DENIED;
}

static EDealRes rewriteTagCond(SNode** pNode, void* pContext) {
  if (QUERY_NODE_COLUMN == nodeType(*pNode)) {
    SRewriteTagCondCxt* pCxt = pContext;
    pCxt->code = rewriteTagCondColumn(pCxt->pTagVals, pCxt->pTagName, pNode);
    return (TSDB_CODE_SUCCESS == pCxt->code ? DEAL_RES_IGNORE_CHILD : DEAL_RES_ERROR);
  }
  return DEAL_RES_CONTINUE;
}

static int32_t setTagVal(SArray* pTagVals, SArray* pTagName, SNode* pCond) {
  SRewriteTagCondCxt cxt = {.code = TSDB_CODE_SUCCESS, .pTagVals = pTagVals, .pTagName = pTagName};
  nodesRewriteExpr(&pCond, rewriteTagCond, &cxt);
  return cxt.code;
}

static int32_t checkTagCondResult(SNode* pResult) {
  return (QUERY_NODE_VALUE == nodeType(pResult) && ((SValueNode*)pResult)->datum.b) ? TSDB_CODE_SUCCESS
                                                                                    : TSDB_CODE_PAR_PERMISSION_DENIED;
}

static int32_t checkSubtablePrivilege(SArray* pTagVals, SArray* pTagName, SNode** pCond) {
  int32_t code = setTagVal(pTagVals, pTagName, *pCond);
  if (TSDB_CODE_SUCCESS == code) {
    code = scalarCalculateConstants(*pCond, pCond);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = checkTagCondResult(*pCond);
  }
  NODES_DESTORY_NODE(*pCond);
  return code;
}

// pSql -> tag1_value, ...)
static int32_t parseTagsClauseImpl(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  int32_t  code = TSDB_CODE_SUCCESS;
  SSchema* pSchema = getTableTagSchema(pStmt->pTableMeta);
  SArray*  pTagVals = taosArrayInit(pCxt->tags.numOfBound, sizeof(STagVal));
  SArray*  pTagName = taosArrayInit(8, TSDB_COL_NAME_LEN);
  SToken   token;
  bool     isParseBindParam = false;
  bool     isJson = false;
  STag*    pTag = NULL;
  for (int i = 0; TSDB_CODE_SUCCESS == code && i < pCxt->tags.numOfBound; ++i) {
    NEXT_TOKEN_WITH_PREV(pStmt->pSql, token);

    if (token.type == TK_NK_QUESTION) {
      isParseBindParam = true;
      if (NULL == pCxt->pComCxt->pStmtCb) {
        code = buildSyntaxErrMsg(&pCxt->msg, "? only used in stmt", token.z);
        break;
      }

      continue;
    }

    if (isParseBindParam) {
      code = buildInvalidOperationMsg(&pCxt->msg, "no mix usage for ? and tag values");
      break;
    }

    SSchema* pTagSchema = &pSchema[pCxt->tags.pColIndex[i]];
    isJson = pTagSchema->type == TSDB_DATA_TYPE_JSON;
    code = checkAndTrimValue(&token, pCxt->tmpTokenBuf, &pCxt->msg);
    if (TSDB_CODE_SUCCESS == code) {
      code = parseTagValue(pCxt, pStmt, pTagSchema, &token, pTagName, pTagVals, &pTag);
    }
  }

  if (TSDB_CODE_SUCCESS == code && NULL != pStmt->pTagCond) {
    code = checkSubtablePrivilege(pTagVals, pTagName, &pStmt->pTagCond);
  }

  if (TSDB_CODE_SUCCESS == code && !isParseBindParam && !isJson) {
    code = tTagNew(pTagVals, 1, false, &pTag);
  }

  if (TSDB_CODE_SUCCESS == code && !isParseBindParam) {
    code = buildCreateTbReq(pStmt, pTag, pTagName);
    pTag = NULL;
  }

  for (int i = 0; i < taosArrayGetSize(pTagVals); ++i) {
    STagVal* p = (STagVal*)taosArrayGet(pTagVals, i);
    if (IS_VAR_DATA_TYPE(p->type)) {
      taosMemoryFreeClear(p->pData);
    }
  }
  taosArrayDestroy(pTagVals);
  taosArrayDestroy(pTagName);
  tTagFree(pTag);
  return code;
}

// input pStmt->pSql:  TAGS (tag1_value, ...) [table_options] ...
// output pStmt->pSql: [table_options] ...
static int32_t parseTagsClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  SToken token;
  NEXT_TOKEN(pStmt->pSql, token);
  if (TK_TAGS != token.type) {
    return buildSyntaxErrMsg(&pCxt->msg, "TAGS is expected", token.z);
  }

  NEXT_TOKEN(pStmt->pSql, token);
  if (TK_NK_LP != token.type) {
    return buildSyntaxErrMsg(&pCxt->msg, "( is expected", token.z);
  }

  int32_t code = parseTagsClauseImpl(pCxt, pStmt);
  if (TSDB_CODE_SUCCESS == code) {
    NEXT_VALID_TOKEN(pStmt->pSql, token);
    if (TK_NK_COMMA == token.type) {
      code = generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_TAGS_NOT_MATCHED);
    } else if (TK_NK_RP != token.type) {
      code = buildSyntaxErrMsg(&pCxt->msg, ") is expected", token.z);
    }
  }
  return code;
}

static int32_t storeTableMeta(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  pStmt->pTableMeta->suid = pStmt->pTableMeta->uid;
  pStmt->pTableMeta->uid = pStmt->totalTbNum;
  pStmt->pTableMeta->tableType = TSDB_CHILD_TABLE;

  STableMeta* pBackup = NULL;
  if (TSDB_CODE_SUCCESS != cloneTableMeta(pStmt->pTableMeta, &pBackup)) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  char tbFName[TSDB_TABLE_FNAME_LEN];
  tNameExtractFullName(&pStmt->targetTableName, tbFName);
  return taosHashPut(pStmt->pSubTableHashObj, tbFName, strlen(tbFName), &pBackup, POINTER_BYTES);
}

static int32_t parseTableOptions(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  do {
    int32_t index = 0;
    SToken  token;
    NEXT_TOKEN_KEEP_SQL(pStmt->pSql, token, index);
    if (TK_TTL == token.type) {
      pStmt->pSql += index;
      NEXT_TOKEN_WITH_PREV(pStmt->pSql, token);
      if (TK_NK_INTEGER != token.type) {
        return buildSyntaxErrMsg(&pCxt->msg, "Invalid option ttl", token.z);
      }
      pStmt->pCreateTblReq->ttl = taosStr2Int32(token.z, NULL, 10);
      if (pStmt->pCreateTblReq->ttl < 0) {
        return buildSyntaxErrMsg(&pCxt->msg, "Invalid option ttl", token.z);
      }
    } else if (TK_COMMENT == token.type) {
      pStmt->pSql += index;
      NEXT_TOKEN(pStmt->pSql, token);
      if (TK_NK_STRING != token.type) {
        return buildSyntaxErrMsg(&pCxt->msg, "Invalid option comment", token.z);
      }
      if (token.n >= TSDB_TB_COMMENT_LEN) {
        return buildSyntaxErrMsg(&pCxt->msg, "comment too long", token.z);
      }
      int32_t len = trimString(token.z, token.n, pCxt->tmpTokenBuf, TSDB_TB_COMMENT_LEN);
      pStmt->pCreateTblReq->comment = strndup(pCxt->tmpTokenBuf, len);
      if (NULL == pStmt->pCreateTblReq->comment) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }
      pStmt->pCreateTblReq->commentLen = len;
    } else {
      break;
    }
  } while (1);
  return TSDB_CODE_SUCCESS;
}

// input pStmt->pSql:
//   1. [(tag1_name, ...)] ...
//   2. VALUES ... | FILE ...
// output pStmt->pSql:
//   1. [(field1_name, ...)]
//   2. VALUES ... | FILE ...
static int32_t parseUsingClauseBottom(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  if (!pStmt->usingTableProcessing || pCxt->usingDuplicateTable) {
    return TSDB_CODE_SUCCESS;
  }

  int32_t code = parseBoundTagsClause(pCxt, pStmt);
  if (TSDB_CODE_SUCCESS == code) {
    code = parseTagsClause(pCxt, pStmt);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = parseTableOptions(pCxt, pStmt);
  }

  return code;
}

static void setUserAuthInfo(SParseContext* pCxt, SName* pTbName, SUserAuthInfo* pInfo) {
  snprintf(pInfo->user, sizeof(pInfo->user), "%s", pCxt->pUser);
  memcpy(&pInfo->tbName, pTbName, sizeof(SName));
  pInfo->type = AUTH_TYPE_WRITE;
}

static int32_t checkAuth(SParseContext* pCxt, SName* pTbName, bool* pMissCache, SNode** pTagCond) {
  int32_t       code = TSDB_CODE_SUCCESS;
  SUserAuthInfo authInfo = {0};
  setUserAuthInfo(pCxt, pTbName, &authInfo);
  SUserAuthRes authRes = {0};
  bool         exists = true;
  if (pCxt->async) {
    code = catalogChkAuthFromCache(pCxt->pCatalog, &authInfo, &authRes, &exists);
  } else {
    SRequestConnInfo conn = {.pTrans = pCxt->pTransporter,
                             .requestId = pCxt->requestId,
                             .requestObjRefId = pCxt->requestRid,
                             .mgmtEps = pCxt->mgmtEpSet};
    code = catalogChkAuth(pCxt->pCatalog, &conn, &authInfo, &authRes);
  }
  if (TSDB_CODE_SUCCESS == code) {
    if (!exists) {
      *pMissCache = true;
    } else if (!authRes.pass) {
      code = TSDB_CODE_PAR_PERMISSION_DENIED;
    } else if (NULL != authRes.pCond) {
      *pTagCond = authRes.pCond;
    }
  }
  return code;
}

static int32_t checkAuthForTable(SParseContext* pCxt, SName* pTbName, bool* pMissCache, bool* pNeedTableTagVal) {
  SNode*  pTagCond = NULL;
  int32_t code = checkAuth(pCxt, pTbName, pMissCache, &pTagCond);
  if (TSDB_CODE_SUCCESS == code) {
    *pNeedTableTagVal = ((*pMissCache) || (NULL != pTagCond));
    *pMissCache = (NULL != pTagCond);
  }
  nodesDestroyNode(pTagCond);
  return code;
}

static int32_t checkAuthForStable(SParseContext* pCxt, SName* pTbName, bool* pMissCache, SNode** pTagCond) {
  return checkAuth(pCxt, pTbName, pMissCache, pTagCond);
}

static int32_t getTableMeta(SInsertParseContext* pCxt, SName* pTbName, bool isStb, STableMeta** pTableMeta,
                            bool* pMissCache) {
  SParseContext* pComCxt = pCxt->pComCxt;
  int32_t        code = TSDB_CODE_SUCCESS;
  if (pComCxt->async) {
    if (isStb) {
      code = catalogGetCachedSTableMeta(pComCxt->pCatalog, pTbName, pTableMeta);
    } else {
      code = catalogGetCachedTableMeta(pComCxt->pCatalog, pTbName, pTableMeta);
    }
  } else {
    SRequestConnInfo conn = {.pTrans = pComCxt->pTransporter,
                             .requestId = pComCxt->requestId,
                             .requestObjRefId = pComCxt->requestRid,
                             .mgmtEps = pComCxt->mgmtEpSet};
    if (isStb) {
      code = catalogGetSTableMeta(pComCxt->pCatalog, &conn, pTbName, pTableMeta);
    } else {
      code = catalogGetTableMeta(pComCxt->pCatalog, &conn, pTbName, pTableMeta);
    }
  }
  if (TSDB_CODE_SUCCESS == code) {
    if (NULL == *pTableMeta) {
      *pMissCache = true;
    } else if (isStb && TSDB_SUPER_TABLE != (*pTableMeta)->tableType) {
      code = buildInvalidOperationMsg(&pCxt->msg, "create table only from super table is allowed");
    } else if (!isStb && TSDB_SUPER_TABLE == (*pTableMeta)->tableType) {
      code = buildInvalidOperationMsg(&pCxt->msg, "insert data into super table is not supported");
    }
  }
  return code;
}

static int32_t getTableVgroup(SParseContext* pCxt, SVnodeModifyOpStmt* pStmt, bool isStb, bool* pMissCache) {
  int32_t     code = TSDB_CODE_SUCCESS;
  SVgroupInfo vg;
  bool        exists = true;
  if (pCxt->async) {
    code = catalogGetCachedTableHashVgroup(pCxt->pCatalog, &pStmt->targetTableName, &vg, &exists);
  } else {
    SRequestConnInfo conn = {.pTrans = pCxt->pTransporter,
                             .requestId = pCxt->requestId,
                             .requestObjRefId = pCxt->requestRid,
                             .mgmtEps = pCxt->mgmtEpSet};
    code = catalogGetTableHashVgroup(pCxt->pCatalog, &conn, &pStmt->targetTableName, &vg);
  }
  if (TSDB_CODE_SUCCESS == code) {
    if (exists) {
      if (isStb) {
        pStmt->pTableMeta->vgId = vg.vgId;
      }
      code = taosHashPut(pStmt->pVgroupsHashObj, (const char*)&vg.vgId, sizeof(vg.vgId), (char*)&vg, sizeof(vg));
    }
    *pMissCache = !exists;
  }
  return code;
}

static int32_t getTableMetaAndVgroupImpl(SParseContext* pCxt, SVnodeModifyOpStmt* pStmt, bool* pMissCache) {
  SVgroupInfo vg;
  int32_t     code = catalogGetCachedTableVgMeta(pCxt->pCatalog, &pStmt->targetTableName, &vg, &pStmt->pTableMeta);
  if (TSDB_CODE_SUCCESS == code) {
    if (NULL != pStmt->pTableMeta) {
      code = taosHashPut(pStmt->pVgroupsHashObj, (const char*)&vg.vgId, sizeof(vg.vgId), (char*)&vg, sizeof(vg));
    }
    *pMissCache = (NULL == pStmt->pTableMeta);
  }
  return code;
}

static int32_t getTableMetaAndVgroup(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, bool* pMissCache) {
  SParseContext* pComCxt = pCxt->pComCxt;
  int32_t        code = TSDB_CODE_SUCCESS;
  if (pComCxt->async) {
    code = getTableMetaAndVgroupImpl(pComCxt, pStmt, pMissCache);
  } else {
    code = getTableMeta(pCxt, &pStmt->targetTableName, false, &pStmt->pTableMeta, pMissCache);
    if (TSDB_CODE_SUCCESS == code && !pCxt->missCache) {
      code = getTableVgroup(pCxt->pComCxt, pStmt, false, &pCxt->missCache);
    }
  }
  return code;
}

static int32_t collectUseTable(const SName* pName, SHashObj* pTable) {
  char fullName[TSDB_TABLE_FNAME_LEN];
  tNameExtractFullName(pName, fullName);
  return taosHashPut(pTable, fullName, strlen(fullName), pName, sizeof(SName));
}

static int32_t collectUseDatabase(const SName* pName, SHashObj* pDbs) {
  char dbFName[TSDB_DB_FNAME_LEN] = {0};
  tNameGetFullDbName(pName, dbFName);
  return taosHashPut(pDbs, dbFName, strlen(dbFName), dbFName, sizeof(dbFName));
}

static int32_t getTargetTableSchema(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  if (pCxt->forceUpdate) {
    pCxt->missCache = true;
    return TSDB_CODE_SUCCESS;
  }

  int32_t code = checkAuthForTable(pCxt->pComCxt, &pStmt->targetTableName, &pCxt->missCache, &pCxt->needTableTagVal);
  if (TSDB_CODE_SUCCESS == code && !pCxt->missCache) {
    code = getTableMetaAndVgroup(pCxt, pStmt, &pCxt->missCache);
  }
  if (TSDB_CODE_SUCCESS == code && !pCxt->pComCxt->async) {
    code = collectUseDatabase(&pStmt->targetTableName, pStmt->pDbFNameHashObj);
    if (TSDB_CODE_SUCCESS == code) {
      code = collectUseTable(&pStmt->targetTableName, pStmt->pTableNameHashObj);
    }
  }
  return code;
}

static int32_t preParseUsingTableName(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, SToken* pTbName) {
  return insCreateSName(&pStmt->usingTableName, pTbName, pCxt->pComCxt->acctId, pCxt->pComCxt->db, &pCxt->msg);
}

static int32_t getUsingTableSchema(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  if (pCxt->forceUpdate) {
    pCxt->missCache = true;
    return TSDB_CODE_SUCCESS;
  }

  int32_t code = checkAuthForStable(pCxt->pComCxt, &pStmt->usingTableName, &pCxt->missCache, &pStmt->pTagCond);
  if (TSDB_CODE_SUCCESS == code && !pCxt->missCache) {
    code = getTableMeta(pCxt, &pStmt->usingTableName, true, &pStmt->pTableMeta, &pCxt->missCache);
  }
  if (TSDB_CODE_SUCCESS == code && !pCxt->missCache) {
    code = getTableVgroup(pCxt->pComCxt, pStmt, true, &pCxt->missCache);
  }
  if (TSDB_CODE_SUCCESS == code && !pCxt->pComCxt->async) {
    code = collectUseDatabase(&pStmt->usingTableName, pStmt->pDbFNameHashObj);
    if (TSDB_CODE_SUCCESS == code) {
      code = collectUseTable(&pStmt->usingTableName, pStmt->pTableNameHashObj);
    }
  }
  return code;
}

static int32_t parseUsingTableNameImpl(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  SToken token;
  NEXT_TOKEN(pStmt->pSql, token);
  int32_t code = preParseUsingTableName(pCxt, pStmt, &token);
  if (TSDB_CODE_SUCCESS == code) {
    code = getUsingTableSchema(pCxt, pStmt);
  }
  if (TSDB_CODE_SUCCESS == code && !pCxt->missCache) {
    code = storeTableMeta(pCxt, pStmt);
  }
  return code;
}

// input pStmt->pSql:
//   1(care). [USING stb_name [(tag1_name, ...)] TAGS (tag1_value, ...) [table_options]] ...
//   2. VALUES ... | FILE ...
// output pStmt->pSql:
//   1. [(tag1_name, ...)] TAGS (tag1_value, ...) [table_options]] ...
//   2. VALUES ... | FILE ...
static int32_t parseUsingTableName(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  SToken  token;
  int32_t index = 0;
  NEXT_TOKEN_KEEP_SQL(pStmt->pSql, token, index);
  if (TK_USING != token.type) {
    return getTargetTableSchema(pCxt, pStmt);
  }

  pStmt->usingTableProcessing = true;
  // pStmt->pSql -> stb_name [(tag1_name, ...)
  pStmt->pSql += index;
  int32_t code = parseDuplicateUsingClause(pCxt, pStmt, &pCxt->usingDuplicateTable);
  if (TSDB_CODE_SUCCESS == code && !pCxt->usingDuplicateTable) {
    return parseUsingTableNameImpl(pCxt, pStmt);
  }
  return code;
}

static int32_t preParseTargetTableName(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, SToken* pTbName) {
  return insCreateSName(&pStmt->targetTableName, pTbName, pCxt->pComCxt->acctId, pCxt->pComCxt->db, &pCxt->msg);
}

// input pStmt->pSql:
//   1(care). [(field1_name, ...)] ...
//   2. [ USING ... ] ...
//   3. VALUES ... | FILE ...
// output pStmt->pSql:
//   1. [ USING ... ] ...
//   2. VALUES ... | FILE ...
static int32_t preParseBoundColumnsClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  SToken  token;
  int32_t index = 0;
  NEXT_TOKEN_KEEP_SQL(pStmt->pSql, token, index);
  if (TK_NK_LP != token.type) {
    return TSDB_CODE_SUCCESS;
  }

  // pStmt->pSql -> field1_name, ...)
  pStmt->pSql += index;
  pStmt->pBoundCols = pStmt->pSql;
  return skipParentheses(pCxt, &pStmt->pSql);
}

static int32_t getTableDataCxt(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, STableDataCxt** pTableCxt) {
  if (pCxt->pComCxt->async) {
    return insGetTableDataCxt(pStmt->pTableBlockHashObj, &pStmt->pTableMeta->uid, sizeof(pStmt->pTableMeta->uid),
                              pStmt->pTableMeta, &pStmt->pCreateTblReq, pTableCxt, false);
  }

  char tbFName[TSDB_TABLE_FNAME_LEN];
  tNameExtractFullName(&pStmt->targetTableName, tbFName);
  if (pStmt->usingTableProcessing) {
    pStmt->pTableMeta->uid = 0;
  }
  return insGetTableDataCxt(pStmt->pTableBlockHashObj, tbFName, strlen(tbFName), pStmt->pTableMeta,
                            &pStmt->pCreateTblReq, pTableCxt, NULL != pCxt->pComCxt->pStmtCb);
}

static int32_t parseBoundColumnsClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, STableDataCxt* pTableCxt) {
  SToken  token;
  int32_t index = 0;
  NEXT_TOKEN_KEEP_SQL(pStmt->pSql, token, index);
  if (TK_NK_LP == token.type) {
    pStmt->pSql += index;
    if (NULL != pStmt->pBoundCols) {
      return buildSyntaxErrMsg(&pCxt->msg, "keyword VALUES or FILE is expected", token.z);
    }
    // pStmt->pSql -> field1_name, ...)
    return parseBoundColumns(pCxt, &pStmt->pSql, false, getTableColumnSchema(pStmt->pTableMeta),
                             &pTableCxt->boundColsInfo);
  }

  if (NULL != pStmt->pBoundCols) {
    return parseBoundColumns(pCxt, &pStmt->pBoundCols, false, getTableColumnSchema(pStmt->pTableMeta),
                             &pTableCxt->boundColsInfo);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t initTableColSubmitData(STableDataCxt* pTableCxt) {
  if (0 == (pTableCxt->pData->flags & SUBMIT_REQ_COLUMN_DATA_FORMAT)) {
    return TSDB_CODE_SUCCESS;
  }

  for (int32_t i = 0; i < pTableCxt->boundColsInfo.numOfBound; ++i) {
    SSchema*  pSchema = &pTableCxt->pMeta->schema[pTableCxt->boundColsInfo.pColIndex[i]];
    SColData* pCol = taosArrayReserve(pTableCxt->pData->aCol, 1);
    if (NULL == pCol) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
    tColDataInit(pCol, pSchema->colId, pSchema->type, 0);
  }

  return TSDB_CODE_SUCCESS;
}

// input pStmt->pSql:
//   1. [(tag1_name, ...)] ...
//   2. VALUES ... | FILE ...
// output pStmt->pSql: VALUES ... | FILE ...
static int32_t parseSchemaClauseBottom(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt,
                                       STableDataCxt** pTableCxt) {
  int32_t code = parseUsingClauseBottom(pCxt, pStmt);
  if (TSDB_CODE_SUCCESS == code) {
    code = getTableDataCxt(pCxt, pStmt, pTableCxt);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = parseBoundColumnsClause(pCxt, pStmt, *pTableCxt);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = initTableColSubmitData(*pTableCxt);
  }
  return code;
}

// input pStmt->pSql: [(field1_name, ...)] [ USING ... ] VALUES ... | FILE ...
// output pStmt->pSql:
//   1. [(tag1_name, ...)] ...
//   2. VALUES ... | FILE ...
static int32_t parseSchemaClauseTop(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, SToken* pTbName) {
  int32_t code = preParseTargetTableName(pCxt, pStmt, pTbName);
  if (TSDB_CODE_SUCCESS == code) {
    // option: [(field1_name, ...)]
    code = preParseBoundColumnsClause(pCxt, pStmt);
  }
  if (TSDB_CODE_SUCCESS == code) {
    // option: [USING stb_name]
    code = parseUsingTableName(pCxt, pStmt);
  }
  return code;
}

static int32_t parseValueTokenImpl(SInsertParseContext* pCxt, const char** pSql, SToken* pToken, SSchema* pSchema,
                                   int16_t timePrec, SColVal* pVal) {
  switch (pSchema->type) {
    case TSDB_DATA_TYPE_BOOL: {
      if ((pToken->type == TK_NK_BOOL || pToken->type == TK_NK_STRING) && (pToken->n != 0)) {
        if (strncmp(pToken->z, "true", pToken->n) == 0) {
          pVal->value.val = TRUE_VALUE;
        } else if (strncmp(pToken->z, "false", pToken->n) == 0) {
          pVal->value.val = FALSE_VALUE;
        } else {
          return buildSyntaxErrMsg(&pCxt->msg, "invalid bool data", pToken->z);
        }
      } else if (pToken->type == TK_NK_INTEGER) {
        pVal->value.val = ((taosStr2Int64(pToken->z, NULL, 10) == 0) ? FALSE_VALUE : TRUE_VALUE);
      } else if (pToken->type == TK_NK_FLOAT) {
        pVal->value.val = ((taosStr2Double(pToken->z, NULL) == 0) ? FALSE_VALUE : TRUE_VALUE);
      } else {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid bool data", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_TINYINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid tinyint data", pToken->z);
      } else if (!IS_VALID_TINYINT(pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "tinyint data overflow", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_UTINYINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid unsigned tinyint data", pToken->z);
      } else if (pVal->value.val > UINT8_MAX) {
        return buildSyntaxErrMsg(&pCxt->msg, "unsigned tinyint data overflow", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_SMALLINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid smallint data", pToken->z);
      } else if (!IS_VALID_SMALLINT(pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "smallint data overflow", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_USMALLINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid unsigned smallint data", pToken->z);
      } else if (pVal->value.val > UINT16_MAX) {
        return buildSyntaxErrMsg(&pCxt->msg, "unsigned smallint data overflow", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_INT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid int data", pToken->z);
      } else if (!IS_VALID_INT(pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "int data overflow", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_UINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid unsigned int data", pToken->z);
      } else if (pVal->value.val > UINT32_MAX) {
        return buildSyntaxErrMsg(&pCxt->msg, "unsigned int data overflow", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_BIGINT: {
      if (TSDB_CODE_SUCCESS != toInteger(pToken->z, pToken->n, 10, &pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid bigint data", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_UBIGINT: {
      if (TSDB_CODE_SUCCESS != toUInteger(pToken->z, pToken->n, 10, &pVal->value.val)) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid unsigned bigint data", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_FLOAT: {
      char*  endptr = NULL;
      double dv;
      if (TK_NK_ILLEGAL == toDouble(pToken, &dv, &endptr)) {
        return buildSyntaxErrMsg(&pCxt->msg, "illegal float data", pToken->z);
      }
      if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || dv > FLT_MAX || dv < -FLT_MAX || isinf(dv) ||
          isnan(dv)) {
        return buildSyntaxErrMsg(&pCxt->msg, "illegal float data", pToken->z);
      }
      float f = dv;
      memcpy(&pVal->value.val, &f, sizeof(f));
      break;
    }
    case TSDB_DATA_TYPE_DOUBLE: {
      char*  endptr = NULL;
      double dv;
      if (TK_NK_ILLEGAL == toDouble(pToken, &dv, &endptr)) {
        return buildSyntaxErrMsg(&pCxt->msg, "illegal double data", pToken->z);
      }
      if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || isinf(dv) || isnan(dv)) {
        return buildSyntaxErrMsg(&pCxt->msg, "illegal double data", pToken->z);
      }
      pVal->value.val = *(int64_t*)&dv;
      break;
    }
    case TSDB_DATA_TYPE_BINARY: {
      // Too long values will raise the invalid sql error message
      if (pToken->n + VARSTR_HEADER_SIZE > pSchema->bytes) {
        return generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
      }
      pVal->value.pData = taosMemoryMalloc(pToken->n);
      if (NULL == pVal->value.pData) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }
      memcpy(pVal->value.pData, pToken->z, pToken->n);
      pVal->value.nData = pToken->n;
      break;
    }
    case TSDB_DATA_TYPE_VARBINARY: {
      int32_t code = parseVarbinary(pToken, &pVal->value.pData, &pVal->value.nData, pSchema->bytes);
      if(code != TSDB_CODE_SUCCESS){
        return generateSyntaxErrMsg(&pCxt->msg, code, pSchema->name);
      }
      break;
    }
    case TSDB_DATA_TYPE_NCHAR: {
      // if the converted output len is over than pColumnModel->bytes, return error: 'Argument list too long'
      int32_t len = 0;
      char*   pUcs4 = taosMemoryCalloc(1, pSchema->bytes - VARSTR_HEADER_SIZE);
      if (NULL == pUcs4) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }
      if (!taosMbsToUcs4(pToken->z, pToken->n, (TdUcs4*)pUcs4, pSchema->bytes - VARSTR_HEADER_SIZE, &len)) {
        taosMemoryFree(pUcs4);
        if (errno == E2BIG) {
          return generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
        }
        char buf[512] = {0};
        snprintf(buf, tListLen(buf), "%s", strerror(errno));
        return buildSyntaxErrMsg(&pCxt->msg, buf, pToken->z);
      }
      pVal->value.pData = pUcs4;
      pVal->value.nData = len;
      break;
    }
    case TSDB_DATA_TYPE_JSON: {
      if (pToken->n > (TSDB_MAX_JSON_TAG_LEN - VARSTR_HEADER_SIZE) / TSDB_NCHAR_SIZE) {
        return buildSyntaxErrMsg(&pCxt->msg, "json string too long than 4095", pToken->z);
      }
      pVal->value.pData = taosMemoryMalloc(pToken->n);
      if (NULL == pVal->value.pData) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }
      memcpy(pVal->value.pData, pToken->z, pToken->n);
      pVal->value.nData = pToken->n;
      break;
    }
    case TSDB_DATA_TYPE_GEOMETRY: {
      int32_t code = TSDB_CODE_FAILED;
      unsigned char *output = NULL;
      size_t size = 0;

      code = parseGeometry(pToken, &output, &size);
      if (code != TSDB_CODE_SUCCESS) {
        code = buildSyntaxErrMsg(&pCxt->msg, getThreadLocalGeosCtx()->errMsg, pToken->z);
      }
      // Too long values will raise the invalid sql error message
      else if (size + VARSTR_HEADER_SIZE > pSchema->bytes) {
        code = generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_VALUE_TOO_LONG, pSchema->name);
      }
      else {
        pVal->value.pData = taosMemoryMalloc(size);
        if (NULL == pVal->value.pData) {
          code = TSDB_CODE_OUT_OF_MEMORY;
        }
        else {
          memcpy(pVal->value.pData, output, size);
          pVal->value.nData = size;
        }
      }

      geosFreeBuffer(output);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      break;
    }
    case TSDB_DATA_TYPE_TIMESTAMP: {
      if (parseTime(pSql, pToken, timePrec, &pVal->value.val, &pCxt->msg) != TSDB_CODE_SUCCESS) {
        return buildSyntaxErrMsg(&pCxt->msg, "invalid timestamp", pToken->z);
      }
      break;
    }
    default:
      return TSDB_CODE_FAILED;
  }

  pVal->flag = CV_FLAG_VALUE;
  return TSDB_CODE_SUCCESS;
}

static int32_t parseValueToken(SInsertParseContext* pCxt, const char** pSql, SToken* pToken, SSchema* pSchema,
                               int16_t timePrec, SColVal* pVal) {
  int32_t code = checkAndTrimValue(pToken, pCxt->tmpTokenBuf, &pCxt->msg);
  if (TSDB_CODE_SUCCESS == code && isNullValue(pSchema->type, pToken)) {
    if (TSDB_DATA_TYPE_TIMESTAMP == pSchema->type && PRIMARYKEY_TIMESTAMP_COL_ID == pSchema->colId) {
      return buildSyntaxErrMsg(&pCxt->msg, "primary timestamp should not be null", pToken->z);
    }
    pVal->flag = CV_FLAG_NULL;
    return TSDB_CODE_SUCCESS;
  }

  if (TSDB_CODE_SUCCESS == code && IS_NUMERIC_TYPE(pSchema->type) && pToken->n == 0) {
    return buildSyntaxErrMsg(&pCxt->msg, "invalid numeric data", pToken->z);
  }

  if (TSDB_CODE_SUCCESS == code) {
    code = parseValueTokenImpl(pCxt, pSql, pToken, pSchema, timePrec, pVal);
  }

  return code;
}

static void clearColValArray(SArray* pCols) {
  int32_t num = taosArrayGetSize(pCols);
  for (int32_t i = 0; i < num; ++i) {
    SColVal* pCol = taosArrayGet(pCols, i);
    if (IS_VAR_DATA_TYPE(pCol->type)) {
      taosMemoryFreeClear(pCol->value.pData);
    }
  }
}

static int parseOneRow(SInsertParseContext* pCxt, const char** pSql, STableDataCxt* pTableCxt, bool* pGotRow,
                       SToken* pToken) {
  SBoundColInfo* pCols = &pTableCxt->boundColsInfo;
  bool           isParseBindParam = false;
  SSchema*       pSchemas = getTableColumnSchema(pTableCxt->pMeta);

  int32_t code = TSDB_CODE_SUCCESS;
  // 1. set the parsed value from sql string
  for (int i = 0; i < pCols->numOfBound && TSDB_CODE_SUCCESS == code; ++i) {
    const char* pOrigSql = *pSql;
    bool        ignoreComma = false;
    NEXT_TOKEN_WITH_PREV_EXT(*pSql, *pToken, &ignoreComma);
    if (ignoreComma) {
      code = buildSyntaxErrMsg(&pCxt->msg, "invalid data or symbol", pOrigSql);
      break;
    }

    SSchema* pSchema = &pSchemas[pCols->pColIndex[i]];
    SColVal* pVal = taosArrayGet(pTableCxt->pValues, pCols->pColIndex[i]);

    if (pToken->type == TK_NK_QUESTION) {
      isParseBindParam = true;
      if (NULL == pCxt->pComCxt->pStmtCb) {
        code = buildSyntaxErrMsg(&pCxt->msg, "? only used in stmt", pToken->z);
        break;
      }
    } else {
      if (TK_NK_RP == pToken->type) {
        code = generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_INVALID_COLUMNS_NUM);
        break;
      }

      if (isParseBindParam) {
        code = buildInvalidOperationMsg(&pCxt->msg, "no mix usage for ? and values");
        break;
      }

      if (TSDB_CODE_SUCCESS == code) {
        code = parseValueToken(pCxt, pSql, pToken, pSchema, getTableInfo(pTableCxt->pMeta).precision, pVal);
      }
    }

    if (TSDB_CODE_SUCCESS == code && i < pCols->numOfBound - 1) {
      NEXT_VALID_TOKEN(*pSql, *pToken);
      if (TK_NK_COMMA != pToken->type) {
        code = buildSyntaxErrMsg(&pCxt->msg, ", expected", pToken->z);
      }
    }
  }

  if (TSDB_CODE_SUCCESS == code && !isParseBindParam) {
    SRow** pRow = taosArrayReserve(pTableCxt->pData->aRowP, 1);
    code = tRowBuild(pTableCxt->pValues, pTableCxt->pSchema, pRow);
    if (TSDB_CODE_SUCCESS == code) {
      insCheckTableDataOrder(pTableCxt, TD_ROW_KEY(*pRow));
    }
  }

  if (TSDB_CODE_SUCCESS == code && !isParseBindParam) {
    *pGotRow = true;
  }

  clearColValArray(pTableCxt->pValues);

  return code;
}

// pSql -> (field1_value, ...) [(field1_value2, ...) ...]
static int32_t parseValues(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, STableDataCxt* pTableCxt,
                           int32_t* pNumOfRows, SToken* pToken) {
  int32_t code = TSDB_CODE_SUCCESS;

  (*pNumOfRows) = 0;
  while (TSDB_CODE_SUCCESS == code) {
    int32_t index = 0;
    NEXT_TOKEN_KEEP_SQL(pStmt->pSql, *pToken, index);
    if (TK_NK_LP != pToken->type) {
      break;
    }
    pStmt->pSql += index;

    bool gotRow = false;
    if (TSDB_CODE_SUCCESS == code) {
      code = parseOneRow(pCxt, &pStmt->pSql, pTableCxt, &gotRow, pToken);
    }

    if (TSDB_CODE_SUCCESS == code) {
      NEXT_VALID_TOKEN(pStmt->pSql, *pToken);
      if (TK_NK_COMMA == pToken->type) {
        code = generateSyntaxErrMsg(&pCxt->msg, TSDB_CODE_PAR_INVALID_COLUMNS_NUM);
      } else if (TK_NK_RP != pToken->type) {
        code = buildSyntaxErrMsg(&pCxt->msg, ") expected", pToken->z);
      }
    }

    if (TSDB_CODE_SUCCESS == code && gotRow) {
      (*pNumOfRows)++;
    }
  }

  if (TSDB_CODE_SUCCESS == code && 0 == (*pNumOfRows) &&
      (!TSDB_QUERY_HAS_TYPE(pStmt->insertType, TSDB_QUERY_TYPE_STMT_INSERT))) {
    code = buildSyntaxErrMsg(&pCxt->msg, "no any data points", NULL);
  }
  return code;
}

// VALUES (field1_value, ...) [(field1_value2, ...) ...]
static int32_t parseValuesClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, STableDataCxt* pTableCxt,
                                 SToken* pToken) {
  int32_t numOfRows = 0;
  int32_t code = parseValues(pCxt, pStmt, pTableCxt, &numOfRows, pToken);
  if (TSDB_CODE_SUCCESS == code) {
    pStmt->totalRowsNum += numOfRows;
    pStmt->totalTbNum += 1;
    TSDB_QUERY_SET_TYPE(pStmt->insertType, TSDB_QUERY_TYPE_INSERT);
  }
  return code;
}

static int32_t parseCsvFile(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, STableDataCxt* pTableCxt,
                            int32_t* pNumOfRows) {
  int32_t code = TSDB_CODE_SUCCESS;
  (*pNumOfRows) = 0;
  char*   pLine = NULL;
  int64_t readLen = 0;
  bool    firstLine = (pStmt->fileProcessing == false);
  pStmt->fileProcessing = false;
  while (TSDB_CODE_SUCCESS == code && (readLen = taosGetLineFile(pStmt->fp, &pLine)) != -1) {
    if (('\r' == pLine[readLen - 1]) || ('\n' == pLine[readLen - 1])) {
      pLine[--readLen] = '\0';
    }

    if (readLen == 0) {
      firstLine = false;
      continue;
    }

    bool gotRow = false;
    if (TSDB_CODE_SUCCESS == code) {
      SToken token;
      strtolower(pLine, pLine);
      const char* pRow = pLine;

      code = parseOneRow(pCxt, (const char**)&pRow, pTableCxt, &gotRow, &token);
      if (code && firstLine) {
        firstLine = false;
        code = 0;
        continue;
      }
    }

    if (TSDB_CODE_SUCCESS == code && gotRow) {
      (*pNumOfRows)++;
    }

    if (TSDB_CODE_SUCCESS == code && (*pNumOfRows) > tsMaxInsertBatchRows) {
      pStmt->fileProcessing = true;
      break;
    }

    firstLine = false;
  }
  taosMemoryFree(pLine);

  parserDebug("0x%" PRIx64 " %d rows have been parsed", pCxt->pComCxt->requestId, *pNumOfRows);

  if (TSDB_CODE_SUCCESS == code && 0 == (*pNumOfRows) &&
      (!TSDB_QUERY_HAS_TYPE(pStmt->insertType, TSDB_QUERY_TYPE_STMT_INSERT)) && !pStmt->fileProcessing) {
    code = buildSyntaxErrMsg(&pCxt->msg, "no any data points", NULL);
  }
  return code;
}

static int32_t parseDataFromFileImpl(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, STableDataCxt* pTableCxt) {
  int32_t numOfRows = 0;
  int32_t code = parseCsvFile(pCxt, pStmt, pTableCxt, &numOfRows);
  if (TSDB_CODE_SUCCESS == code) {
    pStmt->totalRowsNum += numOfRows;
    pStmt->totalTbNum += 1;
    TSDB_QUERY_SET_TYPE(pStmt->insertType, TSDB_QUERY_TYPE_FILE_INSERT);
    if (!pStmt->fileProcessing) {
      taosCloseFile(&pStmt->fp);
    } else {
      parserDebug("0x%" PRIx64 " insert from csv. File is too large, do it in batches.", pCxt->pComCxt->requestId);
    }
  }
  return code;
}

static int32_t parseDataFromFile(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, SToken* pFilePath,
                                 STableDataCxt* pTableCxt) {
  char filePathStr[TSDB_FILENAME_LEN] = {0};
  if (TK_NK_STRING == pFilePath->type) {
    trimString(pFilePath->z, pFilePath->n, filePathStr, sizeof(filePathStr));
  } else {
    strncpy(filePathStr, pFilePath->z, pFilePath->n);
  }
  pStmt->fp = taosOpenFile(filePathStr, TD_FILE_READ | TD_FILE_STREAM);
  if (NULL == pStmt->fp) {
    return TAOS_SYSTEM_ERROR(errno);
  }

  return parseDataFromFileImpl(pCxt, pStmt, pTableCxt);
}

static int32_t parseFileClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, STableDataCxt* pTableCxt,
                               SToken* pToken) {
  if (tsUseAdapter) {
    return buildInvalidOperationMsg(&pCxt->msg, "proxy mode does not support csv loading");
  }

  NEXT_TOKEN(pStmt->pSql, *pToken);
  if (0 == pToken->n || (TK_NK_STRING != pToken->type && TK_NK_ID != pToken->type)) {
    return buildSyntaxErrMsg(&pCxt->msg, "file path is required following keyword FILE", pToken->z);
  }
  return parseDataFromFile(pCxt, pStmt, pToken, pTableCxt);
}

// VALUES (field1_value, ...) [(field1_value2, ...) ...] | FILE csv_file_path
static int32_t parseDataClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, STableDataCxt* pTableCxt) {
  SToken token;
  NEXT_TOKEN(pStmt->pSql, token);
  switch (token.type) {
    case TK_VALUES:
      return parseValuesClause(pCxt, pStmt, pTableCxt, &token);
    case TK_FILE:
      return parseFileClause(pCxt, pStmt, pTableCxt, &token);
    default:
      break;
  }
  return buildSyntaxErrMsg(&pCxt->msg, "keyword VALUES or FILE is expected", token.z);
}

// input pStmt->pSql:
//   1. [(tag1_name, ...)] ...
//   2. VALUES ... | FILE ...
static int32_t parseInsertTableClauseBottom(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  STableDataCxt* pTableCxt = NULL;
  int32_t        code = parseSchemaClauseBottom(pCxt, pStmt, &pTableCxt);
  if (TSDB_CODE_SUCCESS == code) {
    code = parseDataClause(pCxt, pStmt, pTableCxt);
  }
  return code;
}

static void resetEnvPreTable(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  insDestroyBoundColInfo(&pCxt->tags);
  taosMemoryFreeClear(pStmt->pTableMeta);
  nodesDestroyNode(pStmt->pTagCond);
  taosArrayDestroy(pStmt->pTableTag);
  tdDestroySVCreateTbReq(pStmt->pCreateTblReq);
  taosMemoryFreeClear(pStmt->pCreateTblReq);
  pCxt->missCache = false;
  pCxt->usingDuplicateTable = false;
  pStmt->pBoundCols = NULL;
  pStmt->usingTableProcessing = false;
  pStmt->fileProcessing = false;
  pStmt->usingTableName.type = 0;
}

// input pStmt->pSql: [(field1_name, ...)] [ USING ... ] VALUES ... | FILE ...
static int32_t parseInsertTableClause(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, SToken* pTbName) {
  resetEnvPreTable(pCxt, pStmt);
  int32_t code = parseSchemaClauseTop(pCxt, pStmt, pTbName);
  if (TSDB_CODE_SUCCESS == code && !pCxt->missCache) {
    code = parseInsertTableClauseBottom(pCxt, pStmt);
  }
  return code;
}

static int32_t checkTableClauseFirstToken(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, SToken* pTbName,
                                          bool* pHasData) {
  // no data in the sql string anymore.
  if (0 == pTbName->n) {
    if (0 != pTbName->type && '\0' != pStmt->pSql[0]) {
      return buildSyntaxErrMsg(&pCxt->msg, "invalid charactor in SQL", pTbName->z);
    }

    if (0 == pStmt->totalRowsNum && (!TSDB_QUERY_HAS_TYPE(pStmt->insertType, TSDB_QUERY_TYPE_STMT_INSERT))) {
      return buildInvalidOperationMsg(&pCxt->msg, "no data in sql");
    }

    *pHasData = false;
    return TSDB_CODE_SUCCESS;
  }

  if (TSDB_QUERY_HAS_TYPE(pStmt->insertType, TSDB_QUERY_TYPE_STMT_INSERT) && pStmt->totalTbNum > 0) {
    return buildInvalidOperationMsg(&pCxt->msg, "single table allowed in one stmt");
  }

  if (TK_NK_QUESTION == pTbName->type) {
    if (NULL == pCxt->pComCxt->pStmtCb) {
      return buildSyntaxErrMsg(&pCxt->msg, "? only used in stmt", pTbName->z);
    }

    char*   tbName = NULL;
    int32_t code = (*pCxt->pComCxt->pStmtCb->getTbNameFn)(pCxt->pComCxt->pStmtCb->pStmt, &tbName);
    if (TSDB_CODE_SUCCESS == code) {
      pTbName->z = tbName;
      pTbName->n = strlen(tbName);
    } else {
      return code;
    }
  }

  if (TK_NK_ID != pTbName->type && TK_NK_STRING != pTbName->type && TK_NK_QUESTION != pTbName->type) {
    return buildSyntaxErrMsg(&pCxt->msg, "table_name is expected", pTbName->z);
  }

  *pHasData = true;
  return TSDB_CODE_SUCCESS;
}

static int32_t setStmtInfo(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  SBoundColInfo* tags = taosMemoryMalloc(sizeof(pCxt->tags));
  if (NULL == tags) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  memcpy(tags, &pCxt->tags, sizeof(pCxt->tags));

  SStmtCallback* pStmtCb = pCxt->pComCxt->pStmtCb;
  int32_t        code = (*pStmtCb->setInfoFn)(pStmtCb->pStmt, pStmt->pTableMeta, tags, &pStmt->targetTableName,
                                       pStmt->usingTableProcessing, pStmt->pVgroupsHashObj, pStmt->pTableBlockHashObj,
                                       pStmt->usingTableName.tname);

  memset(&pCxt->tags, 0, sizeof(pCxt->tags));
  pStmt->pVgroupsHashObj = NULL;
  pStmt->pTableBlockHashObj = NULL;
  return code;
}

static int32_t parseInsertBodyBottom(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  if (TSDB_QUERY_HAS_TYPE(pStmt->insertType, TSDB_QUERY_TYPE_STMT_INSERT)) {
    return setStmtInfo(pCxt, pStmt);
  }

  // merge according to vgId
  int32_t code = insMergeTableDataCxt(pStmt->pTableBlockHashObj, &pStmt->pVgDataBlocks);
  if (TSDB_CODE_SUCCESS == code) {
    code = insBuildVgDataBlocks(pStmt->pVgroupsHashObj, pStmt->pVgDataBlocks, &pStmt->pDataBlocks);
  }

  return code;
}

// tb_name
//     [USING stb_name [(tag1_name, ...)] TAGS (tag1_value, ...)]
//     [(field1_name, ...)]
//     VALUES (field1_value, ...) [(field1_value2, ...) ...] | FILE csv_file_path
// [...];
static int32_t parseInsertBody(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  SToken  token;
  int32_t code = TSDB_CODE_SUCCESS;
  bool    hasData = true;
  // for each table
  while (TSDB_CODE_SUCCESS == code && hasData && !pCxt->missCache && !pStmt->fileProcessing) {
    // pStmt->pSql -> tb_name ...
    NEXT_TOKEN(pStmt->pSql, token);
    code = checkTableClauseFirstToken(pCxt, pStmt, &token, &hasData);
    if (TSDB_CODE_SUCCESS == code && hasData) {
      code = parseInsertTableClause(pCxt, pStmt, &token);
    }
  }

  if (TSDB_CODE_SUCCESS == code && !pCxt->missCache) {
    code = parseInsertBodyBottom(pCxt, pStmt);
  }
  return code;
}

static void destroySubTableHashElem(void* p) { taosMemoryFree(*(STableMeta**)p); }

static int32_t createVnodeModifOpStmt(SInsertParseContext* pCxt, bool reentry, SNode** pOutput) {
  SVnodeModifyOpStmt* pStmt = (SVnodeModifyOpStmt*)nodesMakeNode(QUERY_NODE_VNODE_MODIFY_STMT);
  if (NULL == pStmt) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  if (pCxt->pComCxt->pStmtCb) {
    TSDB_QUERY_SET_TYPE(pStmt->insertType, TSDB_QUERY_TYPE_STMT_INSERT);
  }
  pStmt->pSql = pCxt->pComCxt->pSql;
  pStmt->freeHashFunc = insDestroyTableDataCxtHashMap;
  pStmt->freeArrayFunc = insDestroyVgroupDataCxtList;

  if (!reentry) {
    pStmt->pVgroupsHashObj = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), true, HASH_NO_LOCK);
    pStmt->pTableBlockHashObj =
        taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), true, HASH_NO_LOCK);
  }
  pStmt->pSubTableHashObj = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_VARCHAR), true, HASH_NO_LOCK);
  pStmt->pTableNameHashObj = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_VARCHAR), true, HASH_NO_LOCK);
  pStmt->pDbFNameHashObj = taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_VARCHAR), true, HASH_NO_LOCK);
  if ((!reentry && (NULL == pStmt->pVgroupsHashObj || NULL == pStmt->pTableBlockHashObj)) ||
      NULL == pStmt->pSubTableHashObj || NULL == pStmt->pTableNameHashObj || NULL == pStmt->pDbFNameHashObj) {
    nodesDestroyNode((SNode*)pStmt);
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  taosHashSetFreeFp(pStmt->pSubTableHashObj, destroySubTableHashElem);

  *pOutput = (SNode*)pStmt;
  return TSDB_CODE_SUCCESS;
}

static int32_t createInsertQuery(SInsertParseContext* pCxt, SQuery** pOutput) {
  SQuery* pQuery = (SQuery*)nodesMakeNode(QUERY_NODE_QUERY);
  if (NULL == pQuery) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  pQuery->execMode = QUERY_EXEC_MODE_SCHEDULE;
  pQuery->haveResultSet = false;
  pQuery->msgType = TDMT_VND_SUBMIT;

  int32_t code = createVnodeModifOpStmt(pCxt, false, &pQuery->pRoot);
  if (TSDB_CODE_SUCCESS == code) {
    *pOutput = pQuery;
  } else {
    nodesDestroyNode((SNode*)pQuery);
  }
  return code;
}

static int32_t checkAuthFromMetaData(const SArray* pUsers, SNode** pTagCond) {
  if (1 != taosArrayGetSize(pUsers)) {
    return TSDB_CODE_FAILED;
  }

  SMetaRes* pRes = taosArrayGet(pUsers, 0);
  if (TSDB_CODE_SUCCESS == pRes->code) {
    SUserAuthRes* pAuth = pRes->pRes;
    if (NULL != pAuth->pCond) {
      *pTagCond = nodesCloneNode(pAuth->pCond);
    }
    return pAuth->pass ? TSDB_CODE_SUCCESS : TSDB_CODE_PAR_PERMISSION_DENIED;
  }
  return pRes->code;
}

static int32_t getTableMetaFromMetaData(const SArray* pTables, STableMeta** pMeta) {
  if (1 != taosArrayGetSize(pTables)) {
    return TSDB_CODE_FAILED;
  }

  taosMemoryFreeClear(*pMeta);
  SMetaRes* pRes = taosArrayGet(pTables, 0);
  if (TSDB_CODE_SUCCESS == pRes->code) {
    *pMeta = tableMetaDup((const STableMeta*)pRes->pRes);
    if (NULL == *pMeta) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
  }
  return pRes->code;
}

static int32_t getTableVgroupFromMetaData(const SArray* pTables, SVnodeModifyOpStmt* pStmt, bool isStb) {
  if (1 != taosArrayGetSize(pTables)) {
    return TSDB_CODE_FAILED;
  }

  SMetaRes* pRes = taosArrayGet(pTables, 0);
  if (TSDB_CODE_SUCCESS != pRes->code) {
    return pRes->code;
  }

  SVgroupInfo* pVg = pRes->pRes;
  if (isStb) {
    pStmt->pTableMeta->vgId = pVg->vgId;
  }
  return taosHashPut(pStmt->pVgroupsHashObj, (const char*)&pVg->vgId, sizeof(pVg->vgId), (char*)pVg,
                     sizeof(SVgroupInfo));
}

static int32_t buildTagNameFromMeta(STableMeta* pMeta, SArray** pTagName) {
  *pTagName = taosArrayInit(pMeta->tableInfo.numOfTags, TSDB_COL_NAME_LEN);
  if (NULL == *pTagName) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  SSchema* pSchema = getTableTagSchema(pMeta);
  for (int32_t i = 0; i < pMeta->tableInfo.numOfTags; ++i) {
    taosArrayPush(*pTagName, pSchema[i].name);
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t checkSubtablePrivilegeForTable(const SArray* pTables, SVnodeModifyOpStmt* pStmt) {
  if (1 != taosArrayGetSize(pTables)) {
    return TSDB_CODE_FAILED;
  }

  SMetaRes* pRes = taosArrayGet(pTables, 0);
  if (TSDB_CODE_SUCCESS != pRes->code) {
    return pRes->code;
  }

  SArray* pTagName = NULL;
  int32_t code = buildTagNameFromMeta(pStmt->pTableMeta, &pTagName);
  if (TSDB_CODE_SUCCESS == code) {
    code = checkSubtablePrivilege((SArray*)pRes->pRes, pTagName, &pStmt->pTagCond);
  }
  taosArrayDestroy(pTagName);
  return code;
}

static int32_t getTableSchemaFromMetaData(SInsertParseContext* pCxt, const SMetaData* pMetaData,
                                          SVnodeModifyOpStmt* pStmt, bool isStb) {
  int32_t code = checkAuthFromMetaData(pMetaData->pUser, &pStmt->pTagCond);
  if (TSDB_CODE_SUCCESS == code) {
    code = getTableMetaFromMetaData(pMetaData->pTableMeta, &pStmt->pTableMeta);
  }
  if (TSDB_CODE_SUCCESS == code && !isStb && TSDB_SUPER_TABLE == pStmt->pTableMeta->tableType) {
    code = buildInvalidOperationMsg(&pCxt->msg, "insert data into super table is not supported");
  }
  if (TSDB_CODE_SUCCESS == code && isStb) {
    code = storeTableMeta(pCxt, pStmt);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = getTableVgroupFromMetaData(pMetaData->pTableHash, pStmt, isStb);
  }
  if (TSDB_CODE_SUCCESS == code && !isStb && NULL != pStmt->pTagCond) {
    code = checkSubtablePrivilegeForTable(pMetaData->pTableTag, pStmt);
  }
  return code;
}

static void destoryTablesReq(void* p) {
  STablesReq* pRes = (STablesReq*)p;
  taosArrayDestroy(pRes->pTables);
}

static void clearCatalogReq(SCatalogReq* pCatalogReq) {
  if (NULL == pCatalogReq) {
    return;
  }

  taosArrayDestroyEx(pCatalogReq->pTableMeta, destoryTablesReq);
  pCatalogReq->pTableMeta = NULL;
  taosArrayDestroyEx(pCatalogReq->pTableHash, destoryTablesReq);
  pCatalogReq->pTableHash = NULL;
  taosArrayDestroy(pCatalogReq->pUser);
  pCatalogReq->pUser = NULL;
  taosArrayDestroy(pCatalogReq->pTableTag);
  pCatalogReq->pTableTag = NULL;
}

static int32_t setVnodeModifOpStmt(SInsertParseContext* pCxt, SCatalogReq* pCatalogReq, const SMetaData* pMetaData,
                                   SVnodeModifyOpStmt* pStmt) {
  clearCatalogReq(pCatalogReq);

  if (pStmt->usingTableProcessing) {
    return getTableSchemaFromMetaData(pCxt, pMetaData, pStmt, true);
  }
  return getTableSchemaFromMetaData(pCxt, pMetaData, pStmt, false);
}

static int32_t resetVnodeModifOpStmt(SInsertParseContext* pCxt, SQuery* pQuery) {
  nodesDestroyNode(pQuery->pRoot);

  int32_t code = createVnodeModifOpStmt(pCxt, true, &pQuery->pRoot);
  if (TSDB_CODE_SUCCESS == code) {
    SVnodeModifyOpStmt* pStmt = (SVnodeModifyOpStmt*)pQuery->pRoot;

    (*pCxt->pComCxt->pStmtCb->getExecInfoFn)(pCxt->pComCxt->pStmtCb->pStmt, &pStmt->pVgroupsHashObj,
                                             &pStmt->pTableBlockHashObj);
    if (NULL == pStmt->pVgroupsHashObj) {
      pStmt->pVgroupsHashObj = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), true, HASH_NO_LOCK);
    }
    if (NULL == pStmt->pTableBlockHashObj) {
      pStmt->pTableBlockHashObj =
          taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), true, HASH_NO_LOCK);
    }
    if (NULL == pStmt->pVgroupsHashObj || NULL == pStmt->pTableBlockHashObj) {
      code = TSDB_CODE_OUT_OF_MEMORY;
    }
  }

  return code;
}

static int32_t initInsertQuery(SInsertParseContext* pCxt, SCatalogReq* pCatalogReq, const SMetaData* pMetaData,
                               SQuery** pQuery) {
  if (NULL == *pQuery) {
    return createInsertQuery(pCxt, pQuery);
  }

  if (NULL != pCxt->pComCxt->pStmtCb) {
    return resetVnodeModifOpStmt(pCxt, *pQuery);
  }

  SVnodeModifyOpStmt* pStmt = (SVnodeModifyOpStmt*)(*pQuery)->pRoot;

  if (!pStmt->fileProcessing) {
    return setVnodeModifOpStmt(pCxt, pCatalogReq, pMetaData, pStmt);
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t setRefreshMate(SQuery* pQuery) {
  SVnodeModifyOpStmt* pStmt = (SVnodeModifyOpStmt*)pQuery->pRoot;

  if (taosHashGetSize(pStmt->pTableNameHashObj) > 0) {
    taosArrayDestroy(pQuery->pTableList);
    pQuery->pTableList = taosArrayInit(taosHashGetSize(pStmt->pTableNameHashObj), sizeof(SName));
    SName* pTable = taosHashIterate(pStmt->pTableNameHashObj, NULL);
    while (NULL != pTable) {
      taosArrayPush(pQuery->pTableList, pTable);
      pTable = taosHashIterate(pStmt->pTableNameHashObj, pTable);
    }
  }

  if (taosHashGetSize(pStmt->pDbFNameHashObj) > 0) {
    taosArrayDestroy(pQuery->pDbList);
    pQuery->pDbList = taosArrayInit(taosHashGetSize(pStmt->pDbFNameHashObj), TSDB_DB_FNAME_LEN);
    char* pDb = taosHashIterate(pStmt->pDbFNameHashObj, NULL);
    while (NULL != pDb) {
      taosArrayPush(pQuery->pDbList, pDb);
      pDb = taosHashIterate(pStmt->pDbFNameHashObj, pDb);
    }
  }

  return TSDB_CODE_SUCCESS;
}

// INSERT INTO
//   tb_name
//       [USING stb_name [(tag1_name, ...)] TAGS (tag1_value, ...) [table_options]]
//       [(field1_name, ...)]
//       VALUES (field1_value, ...) [(field1_value2, ...) ...] | FILE csv_file_path
//   [...];
static int32_t parseInsertSqlFromStart(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  int32_t code = skipInsertInto(&pStmt->pSql, &pCxt->msg);
  if (TSDB_CODE_SUCCESS == code) {
    code = parseInsertBody(pCxt, pStmt);
  }
  return code;
}

static int32_t parseInsertSqlFromCsv(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  STableDataCxt* pTableCxt = NULL;
  int32_t        code = getTableDataCxt(pCxt, pStmt, &pTableCxt);
  if (TSDB_CODE_SUCCESS == code) {
    code = parseDataFromFileImpl(pCxt, pStmt, pTableCxt);
  }

  if (TSDB_CODE_SUCCESS == code) {
    if (pStmt->fileProcessing) {
      code = parseInsertBodyBottom(pCxt, pStmt);
    } else {
      code = parseInsertBody(pCxt, pStmt);
    }
  }

  return code;
}

static int32_t parseInsertSqlFromTable(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  int32_t code = parseInsertTableClauseBottom(pCxt, pStmt);
  if (TSDB_CODE_SUCCESS == code) {
    code = parseInsertBody(pCxt, pStmt);
  }
  return code;
}

static int32_t parseInsertSqlImpl(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt) {
  if (pStmt->pSql == pCxt->pComCxt->pSql || NULL != pCxt->pComCxt->pStmtCb) {
    return parseInsertSqlFromStart(pCxt, pStmt);
  }

  if (pStmt->fileProcessing) {
    return parseInsertSqlFromCsv(pCxt, pStmt);
  }

  return parseInsertSqlFromTable(pCxt, pStmt);
}

static int32_t buildInsertTableReq(SName* pName, SArray** pTables) {
  *pTables = taosArrayInit(1, sizeof(SName));
  if (NULL == *pTables) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  taosArrayPush(*pTables, pName);
  return TSDB_CODE_SUCCESS;
}

static int32_t buildInsertDbReq(SName* pName, SArray** pDbs) {
  if (NULL == *pDbs) {
    *pDbs = taosArrayInit(1, sizeof(STablesReq));
    if (NULL == *pDbs) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
  }

  STablesReq req = {0};
  tNameGetFullDbName(pName, req.dbFName);
  buildInsertTableReq(pName, &req.pTables);
  taosArrayPush(*pDbs, &req);

  return TSDB_CODE_SUCCESS;
}

static int32_t buildInsertUserAuthReq(const char* pUser, SName* pName, SArray** pUserAuth) {
  *pUserAuth = taosArrayInit(1, sizeof(SUserAuthInfo));
  if (NULL == *pUserAuth) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  SUserAuthInfo userAuth = {.type = AUTH_TYPE_WRITE};
  snprintf(userAuth.user, sizeof(userAuth.user), "%s", pUser);
  memcpy(&userAuth.tbName, pName, sizeof(SName));
  taosArrayPush(*pUserAuth, &userAuth);

  return TSDB_CODE_SUCCESS;
}

static int32_t buildInsertTableTagReq(SName* pName, SArray** pTables) { return buildInsertTableReq(pName, pTables); }

static int32_t buildInsertCatalogReq(SInsertParseContext* pCxt, SVnodeModifyOpStmt* pStmt, SCatalogReq* pCatalogReq) {
  int32_t code = buildInsertUserAuthReq(
      pCxt->pComCxt->pUser, (0 == pStmt->usingTableName.type ? &pStmt->targetTableName : &pStmt->usingTableName),
      &pCatalogReq->pUser);
  if (TSDB_CODE_SUCCESS == code && pCxt->needTableTagVal) {
    code = buildInsertTableTagReq(&pStmt->targetTableName, &pCatalogReq->pTableTag);
  }
  if (TSDB_CODE_SUCCESS == code) {
    if (0 == pStmt->usingTableName.type) {
      code = buildInsertDbReq(&pStmt->targetTableName, &pCatalogReq->pTableMeta);
    } else {
      code = buildInsertDbReq(&pStmt->usingTableName, &pCatalogReq->pTableMeta);
    }
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = buildInsertDbReq(&pStmt->targetTableName, &pCatalogReq->pTableHash);
  }
  return code;
}

static int32_t setNextStageInfo(SInsertParseContext* pCxt, SQuery* pQuery, SCatalogReq* pCatalogReq) {
  SVnodeModifyOpStmt* pStmt = (SVnodeModifyOpStmt*)pQuery->pRoot;
  if (pCxt->missCache) {
    parserDebug("0x%" PRIx64 " %d rows of %d tables have been inserted before cache miss", pCxt->pComCxt->requestId,
                pStmt->totalRowsNum, pStmt->totalTbNum);

    pQuery->execStage = QUERY_EXEC_STAGE_PARSE;
    return buildInsertCatalogReq(pCxt, pStmt, pCatalogReq);
  }

  parserDebug("0x%" PRIx64 " %d rows of %d tables have been inserted", pCxt->pComCxt->requestId, pStmt->totalRowsNum,
              pStmt->totalTbNum);

  pQuery->execStage = QUERY_EXEC_STAGE_SCHEDULE;
  return TSDB_CODE_SUCCESS;
}

int32_t parseInsertSql(SParseContext* pCxt, SQuery** pQuery, SCatalogReq* pCatalogReq, const SMetaData* pMetaData) {
  SInsertParseContext context = {.pComCxt = pCxt,
                                 .msg = {.buf = pCxt->pMsg, .len = pCxt->msgLen},
                                 .missCache = false,
                                 .usingDuplicateTable = false,
                                 .forceUpdate = (NULL != pCatalogReq ? pCatalogReq->forceUpdate : false)};

  int32_t code = initInsertQuery(&context, pCatalogReq, pMetaData, pQuery);
  if (TSDB_CODE_SUCCESS == code) {
    code = parseInsertSqlImpl(&context, (SVnodeModifyOpStmt*)(*pQuery)->pRoot);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = setNextStageInfo(&context, *pQuery, pCatalogReq);
  }
  if ((TSDB_CODE_SUCCESS == code || NEED_CLIENT_HANDLE_ERROR(code)) &&
      QUERY_EXEC_STAGE_SCHEDULE == (*pQuery)->execStage) {
    code = setRefreshMate(*pQuery);
  }
  insDestroyBoundColInfo(&context.tags);
  return code;
}
